use std::cell::Cell;
use std::collections::{HashMap, VecDeque};
use std::ffi::OsString;
use std::ffi::c_void;
use std::fs;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use ffmpeg_next as ffmpeg;
use rayon::prelude::*;
use snow_audio_recorder::duration_to_frames_round;
use snow_recording_model::{
    BundleAssetKind, CursorShapeCompositionMode, CursorShapeRecord, MouseStore, RecordingArtifact,
    RecordingBundleFooter, SessionManifest, StoredFrame, decode_mouse_records,
    read_recording_bundle_footer,
};

use crate::config::{
    ExportAudioTrackRequest, ExportExecutionMode, ExportFormat, ExportPerformanceConfig,
    ExportRequest, MouseEditConfig, SoftwareH264Priority, VideoCodec, VideoEncodeConfig,
    VideoEncodingSpeed,
};
use crate::error::{RecordingExportError as ScreenRecorderError, Result};
use crate::export::{
    ExportPathKind, ExportProgress, ExportResult, ExportRuntimeReport, ExportStage,
    ExportStageDurationsMs, ExportTask,
};
use crate::ffmpeg_util::{copy_rgba_into_frame, ensure_ffmpeg_initialized, is_eagain};
use crate::video_quality::{quality_to_h264_crf, smart_quality_bitrate_bps};

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
        ExportRequest {
            playback_speed: 1.0,
            audio_tracks: self
                .manifest
                .audio_tracks
                .iter()
                .map(|track| ExportAudioTrackRequest {
                    track_id: track.track_id.clone(),
                    enabled: false,
                    ..ExportAudioTrackRequest::default()
                })
                .collect(),
            audio_output: crate::config::ExportAudioOutputConfig::default(),
            mouse: crate::config::MouseEditConfig::default(),
            format: ExportFormat::Mp4,
            output_path: self
                .manifest
                .output_dir
                .join(format!("{}.mp4", self.manifest.session_id)),
            video: VideoEncodeConfig {
                quality: 60,
                speed: VideoEncodingSpeed::UltraFast,
            },
            codec: VideoCodec::H264,
            performance: crate::config::ExportPerformanceConfig::default(),
            maximum_width: None,
            maximum_height: None,
            target_fps: None,
        }
    }

    pub fn export(self, request: ExportRequest) -> Result<ExportResult> {
        self.export_with_request(request, None, Arc::new(AtomicBool::new(false)))
    }

    pub fn export_async(self, request: ExportRequest) -> Result<ExportTask> {
        let (progress_tx, progress_rx) = crossbeam_channel::unbounded::<ExportProgress>();
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let cancel_for_worker = Arc::clone(&cancel_flag);
        let handle = thread::Builder::new()
            .name("snow-screen-recorder-export".to_string())
            .spawn(move || self.export_with_request(request, Some(progress_tx), cancel_for_worker))
            .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;

        Ok(ExportTask::new(cancel_flag, progress_rx, handle))
    }

    fn export_with_request(
        self,
        request: ExportRequest,
        progress_tx: Option<Sender<ExportProgress>>,
        cancel_flag: Arc<AtomicBool>,
    ) -> Result<ExportResult> {
        request
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        let request = normalize_export_request(request, &self.manifest);

        self.export_inner(request, &progress_tx, &cancel_flag)
    }

    fn export_inner(
        self,
        mut request: ExportRequest,
        progress_tx: &Option<Sender<ExportProgress>>,
        cancel_flag: &Arc<AtomicBool>,
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

        let mut result = self.export_inner_impl(request, progress_tx, cancel_flag)?;
        staging_path
            .persist(&output_path)
            .map_err(|err| ScreenRecorderError::Io(err.error))?;
        result.output_path = output_path;
        Ok(result)
    }

    fn export_inner_impl(
        self,
        request: ExportRequest,
        progress_tx: &Option<Sender<ExportProgress>>,
        cancel_flag: &Arc<AtomicBool>,
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
        let source_duration_ms = video_index_duration_ms(&source_index);

        let (source_w, source_h) = if self.manifest.width > 0 && self.manifest.height > 0 {
            (self.manifest.width, self.manifest.height)
        } else {
            probe_intermediate_video_dimensions(
                &self.artifact.local_paths.video_intermediate_path,
                self.manifest.width.max(1),
                self.manifest.height.max(1),
            )
        };
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

        if can_use_direct_video_passthrough_copy_path(&self.manifest, &request, needs_overlay) {
            emit_progress(progress_tx, ExportStage::Decode, 2.0, 0.0, None);
            let duration_ms = source_duration_ms;
            match try_direct_video_passthrough_export(
                &self.artifact.local_paths.video_intermediate_path,
                &output_path,
                cancel_flag,
            ) {
                Ok(()) => {
                    runtime_report.path = ExportPathKind::DirectCopy;
                    emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
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
                    // Fall back to remux/re-encode path when direct passthrough cannot be used.
                }
            }
        }

        let retime =
            build_retime_plan_from_index(&source_index, request.playback_speed, export_fps)?;
        if retime.frame_count == 0 {
            return Err(ScreenRecorderError::Export(
                "retiming produced no frames".to_string(),
            ));
        }

        let duration_ms = retime.output_duration_ms.max(1);

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

        if can_use_video_packet_copy_path(
            request.format,
            request.playback_speed,
            self.manifest.fps,
            export_fps,
            needs_overlay,
            needs_resize,
            request.codec == VideoCodec::H264,
        ) {
            match export_video_packet_copy_with_generated_audio(
                &self.artifact.local_paths.video_intermediate_path,
                &output_path,
                request.format,
                request.playback_speed,
                mixed_audio.as_ref(),
                request.audio_output.bitrate_kbps.max(8),
                &request.performance,
                cancel_flag,
                progress_tx,
            ) {
                Ok(telemetry) => {
                    runtime_report.path = ExportPathKind::PacketCopy;
                    apply_codec_telemetry(&mut runtime_report, telemetry);
                    emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
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
                    // Fall back to full decode/compose/re-encode path when remuxing is unsupported.
                }
            }
        }

        if !needs_overlay {
            emit_progress(progress_tx, ExportStage::Decode, 20.0, 0.0, None);
            let telemetry = match export_video_generated_from_source(
                &self.artifact.local_paths.video_intermediate_path,
                &output_path,
                &retime,
                output_w,
                output_h,
                export_fps,
                request.format,
                request.codec,
                mixed_audio.as_ref(),
                request.audio_output.bitrate_kbps.max(8),
                &request.video,
                &request.performance,
                cancel_flag,
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
            emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
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
            mixed_audio.as_ref(),
            request.audio_output.bitrate_kbps.max(8),
            &request.video,
            &request.performance,
            overlay_tracks_ref,
            &request.mouse,
            cancel_flag,
            progress_tx,
        ) {
            Ok(telemetry) => {
                runtime_report.path = ExportPathKind::FullTranscode;
                apply_codec_telemetry(&mut runtime_report, telemetry);
                emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));
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
        let required_source_indices = collect_required_source_indices(&retime.source_indices);
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
            retime.frame_count,
            export_fps,
            request.format,
            request.codec,
            mixed_audio.as_ref(),
            request.audio_output.bitrate_kbps.max(8),
            &request.video,
            &request.performance,
            cancel_flag,
            progress_tx,
            |index, output_rgba| {
                check_canceled(cancel_flag)?;
                debug_assert_eq!(output_rgba.len(), output_len);

                let src_idx = retime.source_indices[index];
                let source = frame_source.frame_at(src_idx)?;
                let output_ts = retime.output_timestamps_ms[index];
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
        emit_progress(progress_tx, ExportStage::Finalize, 100.0, 0.0, Some(0));

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

fn output_dimensions(
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

fn can_use_video_packet_copy_path(
    format: ExportFormat,
    playback_speed: f32,
    source_fps: u32,
    export_fps: u32,
    needs_overlay: bool,
    needs_resize: bool,
    codec_matches_source: bool,
) -> bool {
    !format.is_animated_image()
        && !needs_overlay
        && !needs_resize
        && codec_matches_source
        && playback_speed.is_finite()
        && playback_speed > 0.0
        && source_fps.max(1) == export_fps.max(1)
}

#[inline]
fn hardware_video_encode_allowed(mode: ExportExecutionMode) -> bool {
    matches!(
        mode,
        ExportExecutionMode::HardwarePreferred | ExportExecutionMode::HardwareOnly
    )
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
) -> Result<Option<(ffmpeg::decoder::Video, HardwareDecodeState)>> {
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

        let mut device_ctx = ptr::null_mut();
        let create_status = unsafe {
            ffmpeg::ffi::av_hwdevice_ctx_create(
                &mut device_ctx,
                *device_type,
                ptr::null(),
                ptr::null_mut(),
                0,
            )
        };
        if create_status < 0 || device_ctx.is_null() {
            continue;
        }

        let mut selection = Box::new(HardwareDecodeSelection { hw_pix_fmt });
        let mut decode_context = ffmpeg::codec::context::Context::from_parameters(
            parameters.clone(),
        )
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

        unsafe {
            let codec_ctx = decode_context.as_mut_ptr();
            (*codec_ctx).get_format = Some(select_hardware_decoder_pixel_format);
            (*codec_ctx).opaque = selection.as_mut() as *mut HardwareDecodeSelection as *mut c_void;
            (*codec_ctx).hw_device_ctx = ffmpeg::ffi::av_buffer_ref(device_ctx);
        }

        let decoder = match decode_context
            .decoder()
            .open_as(codec)
            .and_then(|opened| opened.video())
        {
            Ok(decoder) => decoder,
            Err(_) => {
                unsafe {
                    ffmpeg::ffi::av_buffer_unref(&mut device_ctx);
                }
                continue;
            }
        };

        return Ok(Some((
            decoder,
            HardwareDecodeState {
                device_ctx,
                _selection: selection,
                hw_pixel_format: ffmpeg::format::Pixel::from(hw_pix_fmt),
                device_name,
            },
        )));
    }

    Ok(None)
}

fn open_source_video_decoder(
    parameters: &ffmpeg::codec::Parameters,
    perf_config: &ExportPerformanceConfig,
    allow_hardware_decode: bool,
) -> Result<(ffmpeg::decoder::Video, Option<HardwareDecodeState>)> {
    if allow_hardware_decode
        && let Some((decoder, hw_state)) = try_open_hardware_video_decoder(parameters, perf_config)?
    {
        return Ok((decoder, Some(hw_state)));
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
    Ok((decoder, None))
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

fn can_use_direct_video_passthrough_copy_path(
    manifest: &SessionManifest,
    request: &ExportRequest,
    needs_overlay: bool,
) -> bool {
    if request.audio_tracks.iter().any(|track| track.enabled) {
        return false;
    }
    if (request.playback_speed - 1.0).abs() > f32::EPSILON {
        return false;
    }
    if !matches!(request.format, ExportFormat::Mp4) {
        return false;
    }
    if request.codec != VideoCodec::H264
        || request.target_fps.is_some()
        || request.maximum_width.is_some()
        || request.maximum_height.is_some()
    {
        return false;
    }
    if !path_has_extension(&request.output_path, "mp4") {
        return false;
    }

    can_use_video_packet_copy_path(
        request.format,
        request.playback_speed,
        manifest.fps,
        manifest.fps,
        needs_overlay,
        false,
        true,
    )
}

fn path_has_extension(path: &Path, expected: &str) -> bool {
    path.extension()
        .and_then(|value| value.to_str())
        .map(|value| value.eq_ignore_ascii_case(expected))
        .unwrap_or(false)
}

fn try_direct_video_passthrough_export(
    input_video_path: &Path,
    output_path: &Path,
    cancel_flag: &Arc<AtomicBool>,
) -> Result<()> {
    check_canceled(cancel_flag)?;
    if input_video_path == output_path {
        return Err(ScreenRecorderError::Export(
            "output path must differ from intermediate video path for direct passthrough export"
                .to_string(),
        ));
    }

    let _ = fs::remove_file(output_path);
    if fs::hard_link(input_video_path, output_path).is_err() {
        fs::copy(input_video_path, output_path).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to copy passthrough source video {} into {}: {err}",
                input_video_path.display(),
                output_path.display()
            ))
        })?;
    }

    check_canceled(cancel_flag)?;
    Ok(())
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

#[inline]
fn auto_thread_count_from_physical_cores() -> usize {
    let logical = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .max(1);
    let physical = num_cpus::get_physical();
    if physical == 0 {
        logical
    } else {
        physical.min(logical).max(1)
    }
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

fn emit_progress(
    progress_tx: &Option<Sender<ExportProgress>>,
    stage: ExportStage,
    percent: f32,
    video_fps: f32,
    eta_ms: Option<u64>,
) {
    if let Some(tx) = progress_tx {
        let _ = tx.send(ExportProgress {
            stage,
            percent: percent.clamp(0.0, 100.0),
            video_fps: video_fps.max(0.0),
            eta_ms,
            queue_utilization: 0.0,
            peak_memory_mb: 0,
        });
    }
}

#[derive(Clone, Debug)]
struct RetimePlan {
    source_indices: Vec<usize>,
    output_timestamps_ms: Vec<u64>,
    frame_count: usize,
    output_duration_ms: u64,
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
            "cannot retime an empty source frame sequence".to_string(),
        ));
    }

    let mut starts = Vec::with_capacity(source_index.len());
    let mut accumulated = 0u64;
    for entry in source_index {
        starts.push(entry.timestamp_ms);
        accumulated = accumulated.max(
            entry
                .timestamp_ms
                .saturating_add(u64::from(entry.duration_ms.max(1))),
        );
    }
    if accumulated == 0 {
        return Err(ScreenRecorderError::Export(
            "source frame duration is zero".to_string(),
        ));
    }

    let speed = playback_speed.clamp(0.25, 4.0);
    let interval_ms = (1000.0 / export_fps.max(1) as f64).max(1.0);
    let output_duration_ms = ((accumulated as f64) / speed as f64).ceil().max(1.0) as u64;
    let frame_count = ((output_duration_ms as f64) / interval_ms).ceil().max(1.0) as usize;
    let mut source_indices = Vec::with_capacity(frame_count);
    let mut output_timestamps_ms = Vec::with_capacity(frame_count);
    let mut src_idx = 0usize;

    for frame_idx in 0..frame_count {
        let output_ts = (frame_idx as f64 * interval_ms).round() as u64;
        let source_ts = (output_ts as f64 * speed as f64).round() as u64;
        while src_idx + 1 < starts.len() && source_ts >= starts[src_idx + 1] {
            src_idx += 1;
        }
        source_indices.push(src_idx);
        output_timestamps_ms.push(output_ts);
    }

    Ok(RetimePlan {
        source_indices,
        output_timestamps_ms,
        frame_count,
        output_duration_ms,
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

struct StreamingVideoFrameSource {
    rx: Receiver<DecodedFrameMessage>,
    recycle_tx: Sender<Vec<u8>>,
    current_index: Option<usize>,
    current_frame: Option<StoredFrame>,
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
        std::thread::Builder::new()
            .name("snow-screen-recorder-export-decode".to_string())
            .spawn(move || {
                decode_video_stream_worker(
                    path,
                    required_indices,
                    fallback_fps,
                    decode_threads,
                    cancel_flag,
                    tx,
                    recycle_rx,
                )
            })
            .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;
        Ok(Self {
            rx,
            recycle_tx,
            current_index: None,
            current_frame: None,
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
        let msg = self.rx.recv().map_err(|_| {
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

fn decode_video_stream_worker(
    path: std::path::PathBuf,
    required_indices: Vec<usize>,
    fallback_fps: u32,
    decode_threads: u8,
    cancel_flag: Arc<AtomicBool>,
    tx: crossbeam_channel::Sender<DecodedFrameMessage>,
    recycle_rx: crossbeam_channel::Receiver<Vec<u8>>,
) {
    let result = (|| -> Result<()> {
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
            if cancel_flag.load(Ordering::Acquire) {
                return Err(ScreenRecorderError::ExportCanceled);
            }
            if stream.index() != stream_index {
                continue;
            }
            decoder.send_packet(&packet).map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to feed packet into temporary recording video decoder: {err}"
                ))
            })?;
            loop {
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

        decoder.send_eof().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to flush temporary recording video decoder: {err}"
            ))
        })?;
        loop {
            if cancel_flag.load(Ordering::Acquire) {
                return Err(ScreenRecorderError::ExportCanceled);
            }
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

fn collect_required_source_indices(source_indices: &[usize]) -> Vec<usize> {
    let mut required = Vec::with_capacity(source_indices.len());
    for &index in source_indices {
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

#[derive(Clone, Debug)]
struct NearestResizePlan {
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
    src_x_byte_offsets: Vec<usize>,
    src_row_byte_offsets: Vec<usize>,
}

impl NearestResizePlan {
    fn new(src_w: u32, src_h: u32, dst_w: u32, dst_h: u32) -> Self {
        let src_w_usize = src_w.max(1) as usize;
        let src_row_bytes = src_w_usize * 4;
        let src_x_byte_offsets = (0..dst_w.max(1))
            .map(|x| {
                let sx = ((x as u64 * src_w.max(1) as u64) / dst_w.max(1) as u64) as usize;
                sx * 4
            })
            .collect();
        let src_row_byte_offsets = (0..dst_h.max(1))
            .map(|y| {
                let sy = ((y as u64 * src_h.max(1) as u64) / dst_h.max(1) as u64) as usize;
                sy * src_row_bytes
            })
            .collect();

        Self {
            src_w: src_w.max(1),
            src_h: src_h.max(1),
            dst_w: dst_w.max(1),
            dst_h: dst_h.max(1),
            src_x_byte_offsets,
            src_row_byte_offsets,
        }
    }
}

fn resize_rgba_fast_into(
    src: &[u8],
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
    plan: Option<&NearestResizePlan>,
    out: &mut [u8],
    process_pool: Option<&rayon::ThreadPool>,
) {
    if src_w == dst_w && src_h == dst_h {
        out.copy_from_slice(src);
        return;
    }

    let expected = dst_w.max(1) as usize * dst_h.max(1) as usize * 4;
    debug_assert_eq!(out.len(), expected);

    if let Some(plan) = plan
        && plan.src_w == src_w.max(1)
        && plan.src_h == src_h.max(1)
        && plan.dst_w == dst_w.max(1)
        && plan.dst_h == dst_h.max(1)
    {
        resize_rgba_with_plan(src, plan, out, process_pool);
        return;
    }

    resize_rgba_scalar(src, src_w, src_h, dst_w, dst_h, out);
}

fn resize_rgba_with_plan(
    src: &[u8],
    plan: &NearestResizePlan,
    out: &mut [u8],
    process_pool: Option<&rayon::ThreadPool>,
) {
    let dst_row_bytes = plan.dst_w as usize * 4;
    let should_parallel = process_pool.is_some()
        && (plan.dst_w as usize * plan.dst_h as usize) >= 1_000_000
        && plan.dst_h >= 256;
    if should_parallel {
        let pool = process_pool.expect("checked Some above");
        pool.install(|| {
            out.par_chunks_exact_mut(dst_row_bytes)
                .enumerate()
                .for_each(|(y, row)| {
                    let src_row = plan.src_row_byte_offsets[y];
                    let row_u32 = row.as_mut_ptr() as *mut u32;
                    for (dst_x, src_x) in plan.src_x_byte_offsets.iter().enumerate() {
                        let src_offset = src_row + *src_x;
                        // SAFETY:
                        // - Source and destination pixel addresses are valid by plan construction.
                        // - Each parallel worker owns disjoint `row` slices.
                        unsafe {
                            let pixel =
                                ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                            ptr::write_unaligned(row_u32.add(dst_x), pixel);
                        }
                    }
                });
        });
        return;
    }

    for (y, row) in out.chunks_exact_mut(dst_row_bytes).enumerate() {
        let src_row = plan.src_row_byte_offsets[y];
        let row_u32 = row.as_mut_ptr() as *mut u32;
        for (dst_x, src_x) in plan.src_x_byte_offsets.iter().enumerate() {
            let src_offset = src_row + *src_x;
            // SAFETY:
            // - Source and destination pixel addresses are valid by plan construction.
            unsafe {
                let pixel = ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                ptr::write_unaligned(row_u32.add(dst_x), pixel);
            }
        }
    }
}

fn resize_rgba_scalar(src: &[u8], src_w: u32, src_h: u32, dst_w: u32, dst_h: u32, out: &mut [u8]) {
    let dst_row_bytes = dst_w.max(1) as usize * 4;
    for (y, row) in out.chunks_exact_mut(dst_row_bytes).enumerate() {
        let sy = ((y as u64 * src_h.max(1) as u64) / dst_h.max(1) as u64) as usize;
        let src_row = sy * src_w.max(1) as usize * 4;
        let row_u32 = row.as_mut_ptr() as *mut u32;
        for x in 0..dst_w.max(1) as usize {
            let sx = ((x as u64 * src_w.max(1) as u64) / dst_w.max(1) as u64) as usize;
            let src_offset = src_row + sx * 4;
            // SAFETY:
            // - Source and destination pixel addresses are valid by loop bounds.
            unsafe {
                let pixel = ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                ptr::write_unaligned(row_u32.add(x), pixel);
            }
        }
    }
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
    Fallback,
    Plan(CompiledCursorShapePlan),
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
        return CompiledCursorShape::Fallback;
    }

    let mut rgba = shape.shape_rgba;
    rgba.truncate(expected_len);
    let rgba = rgba.into_boxed_slice();
    let yuva = rgba_to_yuva(&rgba);
    let Some((rows, yuv420p_compatible)) = compile_cursor_rows(shape.mode, &rgba, width, height)
    else {
        return CompiledCursorShape::Fallback;
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
        has_cursor: config.visible && current.visible,
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

#[allow(dead_code)]
fn draw_mouse_trail(
    surface: &mut FrameSurfaceMut<'_>,
    samples: &[MouseSample],
    ts: u64,
    config: &MouseEditConfig,
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
    draw_mouse_trail_points(surface, &points, ts, config);
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
    if let Some(shape_id) = current.shape_id
        && let Some(CompiledCursorShape::Plan(shape)) = tracks.cursor_shapes.get(&shape_id)
    {
        draw_compiled_cursor_shape(
            surface,
            current.x.saturating_sub(shape.hotspot_x),
            current.y.saturating_sub(shape.hotspot_y),
            shape,
        );
        return;
    }

    draw_fallback_cursor(surface, current.x, current.y);
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
    if let Some(shape_id) = current.shape_id
        && let Some(shape) = tracks.cursor_shapes.get(&shape_id)
        && let CompiledCursorShape::Plan(shape) = shape
        && shape.yuv420p_compatible
    {
        draw_compiled_cursor_shape_native(
            surface,
            current.x.saturating_sub(shape.hotspot_x),
            current.y.saturating_sub(shape.hotspot_y),
            shape,
        );
        return;
    }

    draw_fallback_cursor_native(surface, current.x, current.y);
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

fn draw_line_native(
    surface: &mut NativeFrameViewMut<'_>,
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
    color: YuvBlendColor,
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
                set_native_pixel_blended(surface, x0 + ox, y0 + oy, color);
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

fn fill_triangle_native(
    surface: &mut NativeFrameViewMut<'_>,
    points: [(i32, i32); 3],
    color: YuvBlendColor,
) {
    let min_x = points.iter().map(|p| p.0).min().unwrap_or(0);
    let max_x = points.iter().map(|p| p.0).max().unwrap_or(0);
    let min_y = points.iter().map(|p| p.1).min().unwrap_or(0);
    let max_y = points.iter().map(|p| p.1).max().unwrap_or(0);

    let area = edge(points[0], points[1], points[2]) as f32;
    if area.abs() < f32::EPSILON {
        return;
    }

    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let p = (x, y);
            let w0 = edge(points[1], points[2], p) as f32 / area;
            let w1 = edge(points[2], points[0], p) as f32 / area;
            let w2 = edge(points[0], points[1], p) as f32 / area;
            if w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0 {
                set_native_pixel_blended(surface, x, y, color);
            }
        }
    }
}

fn draw_fallback_cursor_native(surface: &mut NativeFrameViewMut<'_>, x: i32, y: i32) {
    let points = [(x, y), (x + 12, y + 4), (x + 4, y + 12)];
    let fill = YuvBlendColor::from_rgba([255, 255, 255, 235]);
    let stroke = YuvBlendColor::from_rgba([0, 0, 0, 200]);
    fill_triangle_native(surface, points, fill);
    draw_line_native(surface, x, y, x + 12, y + 4, stroke, 1);
    draw_line_native(surface, x, y, x + 4, y + 12, stroke, 1);
    draw_line_native(surface, x + 12, y + 4, x + 4, y + 12, stroke, 1);
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

fn ensure_video_frame_writable(frame: &mut ffmpeg::frame::Video) -> Result<()> {
    let status = unsafe { ffmpeg::ffi::av_frame_make_writable(frame.as_mut_ptr()) };
    if status < 0 {
        return Err(ScreenRecorderError::Export(format!(
            "failed to make video frame writable: {}",
            ffmpeg::Error::from(status)
        )));
    }
    Ok(())
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

fn draw_fallback_cursor(surface: &mut FrameSurfaceMut<'_>, x: i32, y: i32) {
    let points = [(x, y), (x + 12, y + 4), (x + 4, y + 12)];
    fill_triangle(surface, points, [255, 255, 255, 235]);
    draw_line(surface, x, y, x + 12, y + 4, [0, 0, 0, 200], 1);
    draw_line(surface, x, y, x + 4, y + 12, [0, 0, 0, 200], 1);
    draw_line(surface, x + 12, y + 4, x + 4, y + 12, [0, 0, 0, 200], 1);
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

fn set_pixel_blended(rgba: &mut [u8], width: u32, height: u32, x: i32, y: i32, color: [u8; 4]) {
    let Some(idx) = pixel_offset(width, height, x, y) else {
        return;
    };
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
                set_pixel_blended(
                    surface.rgba,
                    surface.width,
                    surface.height,
                    x0 + ox,
                    y0 + oy,
                    color,
                );
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
            set_pixel_blended(
                surface.rgba,
                surface.width,
                surface.height,
                cx + dx,
                cy + dy,
                color,
            );
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

fn fill_triangle(surface: &mut FrameSurfaceMut<'_>, points: [(i32, i32); 3], color: [u8; 4]) {
    let min_x = points.iter().map(|p| p.0).min().unwrap_or(0);
    let max_x = points.iter().map(|p| p.0).max().unwrap_or(0);
    let min_y = points.iter().map(|p| p.1).min().unwrap_or(0);
    let max_y = points.iter().map(|p| p.1).max().unwrap_or(0);

    let area = edge(points[0], points[1], points[2]) as f32;
    if area.abs() < f32::EPSILON {
        return;
    }

    for y in min_y..=max_y {
        for x in min_x..=max_x {
            let p = (x, y);
            let w0 = edge(points[1], points[2], p) as f32 / area;
            let w1 = edge(points[2], points[0], p) as f32 / area;
            let w2 = edge(points[0], points[1], p) as f32 / area;
            if w0 >= 0.0 && w1 >= 0.0 && w2 >= 0.0 {
                set_pixel_blended(surface.rgba, surface.width, surface.height, x, y, color);
            }
        }
    }
}

fn edge(a: (i32, i32), b: (i32, i32), p: (i32, i32)) -> i32 {
    (p.0 - a.0) * (b.1 - a.1) - (p.1 - a.1) * (b.0 - a.0)
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
    let channels_usize = usize::from(track_manifest.channels.max(1));
    let aligned_sample_count = (sample_count / channels_usize) * channels_usize;
    let frame_count = aligned_sample_count / channels_usize;
    if frame_count == 0 {
        return Ok(None);
    }

    Ok(Some(AudioTrackPlan {
        asset_offset: asset.offset,
        frame_count,
        volume: volume.clamp(0.0, 2.0),
    }))
}

const AUDIO_RENDER_CACHE_FRAMES: usize = 16_384;
#[allow(dead_code)]
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

    #[allow(dead_code)]
    fn render_into_f32_packed(
        &mut self,
        start_output_frame: usize,
        frame_count: usize,
        out: &mut [f32],
    ) -> Result<()> {
        let expected_samples = frame_count.saturating_mul(self.channels_usize);
        if out.len() < expected_samples {
            return Err(ScreenRecorderError::Export(
                "audio render target buffer is too small".to_string(),
            ));
        }
        out[..expected_samples].fill(0.0);
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
                self.render_direct_single_track_f32_packed(
                    start_output_frame,
                    active_frame_count,
                    &mut out[..expected_samples],
                )?;
            } else {
                self.render_direct_mixed_tracks_f32_packed(
                    start_output_frame,
                    active_frame_count,
                    &mut out[..expected_samples],
                )?;
            }
            return Ok(());
        }

        if self.readers.len() == 1 {
            self.render_retimed_single_track_f32_packed(
                start_output_frame,
                active_frame_count,
                &mut out[..expected_samples],
            )
        } else {
            self.render_retimed_tracks_f32_packed(
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

    #[allow(dead_code)]
    fn render_direct_single_track_f32_packed(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [f32],
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
        let cache = &reader.cache_samples_i16[track_start..track_start + sample_count];
        let volume = reader.plan.volume * I16_TO_F32_PCM_SCALE;
        if (reader.plan.volume - 1.0).abs() < f32::EPSILON {
            for (dst, src) in out[..sample_count].iter_mut().zip(cache.iter().copied()) {
                *dst = src as f32 * I16_TO_F32_PCM_SCALE;
            }
        } else {
            for (dst, src) in out[..sample_count].iter_mut().zip(cache.iter().copied()) {
                *dst = src as f32 * volume;
            }
        }
        Ok(())
    }

    #[allow(dead_code)]
    fn render_direct_mixed_tracks_f32_packed(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [f32],
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
                out[out_base + channel_index] = mixed_sample_normalized(acc, self.readers.len());
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

    #[allow(dead_code)]
    fn render_retimed_single_track_f32_packed(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [f32],
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
        let volume = reader.plan.volume * I16_TO_F32_PCM_SCALE;
        let apply_volume = (reader.plan.volume - 1.0).abs() >= f32::EPSILON;

        let mut src_pos = start_output_frame as f64 * speed;
        for frame_offset in 0..active_frame_count {
            let src_index = src_pos.floor() as usize;
            let next_index = (src_index + 1).min(max_source_frame);
            let frac = (src_pos - src_index as f64) as f32;
            let out_base = frame_offset * self.channels_usize;
            let src_base = (src_index - cache_start_frame) * self.channels_usize;

            if src_index == next_index || frac <= f32::EPSILON {
                if !apply_volume {
                    for channel_index in 0..self.channels_usize {
                        out[out_base + channel_index] =
                            cache[src_base + channel_index] as f32 * I16_TO_F32_PCM_SCALE;
                    }
                } else {
                    for channel_index in 0..self.channels_usize {
                        out[out_base + channel_index] =
                            cache[src_base + channel_index] as f32 * volume;
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
                    interpolated *= reader.plan.volume;
                }
                out[out_base + channel_index] = interpolated * I16_TO_F32_PCM_SCALE;
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

    #[allow(dead_code)]
    fn render_retimed_tracks_f32_packed(
        &mut self,
        start_output_frame: usize,
        active_frame_count: usize,
        out: &mut [f32],
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
                out[out_base + channel_index] = mixed_sample_normalized(acc, self.readers.len());
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

fn choose_video_codec_id(format: ExportFormat, video_codec: VideoCodec) -> ffmpeg::codec::Id {
    match format {
        ExportFormat::Mp4 => match video_codec {
            VideoCodec::H264 => ffmpeg::codec::Id::H264,
            VideoCodec::H265 => ffmpeg::codec::Id::HEVC,
        },
        ExportFormat::Avi => ffmpeg::codec::Id::MPEG4,
        ExportFormat::Gif => ffmpeg::codec::Id::GIF,
        ExportFormat::Apng => ffmpeg::codec::Id::APNG,
        // FFmpeg 9's animated libwebp encoder advertises AV_CODEC_ID_WEBP;
        // AV_CODEC_ID_WEBP_ANIM is used only by the native decoder.
        ExportFormat::Webp => ffmpeg::codec::Id::WEBP,
    }
}

fn choose_video_pixel_format(
    format: ExportFormat,
    codec: ffmpeg::codec::Video,
    source_hint: Option<ffmpeg::format::Pixel>,
    mode: ExportExecutionMode,
) -> ffmpeg::format::Pixel {
    let preferred = match format {
        ExportFormat::Gif => [
            ffmpeg::format::Pixel::RGB8,
            ffmpeg::format::Pixel::PAL8,
            ffmpeg::format::Pixel::RGB24,
        ]
        .as_slice(),
        ExportFormat::Apng => [
            ffmpeg::format::Pixel::RGBA,
            ffmpeg::format::Pixel::RGB24,
            ffmpeg::format::Pixel::BGRA,
        ]
        .as_slice(),
        ExportFormat::Webp => [
            ffmpeg::format::Pixel::YUVA420P,
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::RGBA,
        ]
        .as_slice(),
        ExportFormat::Mp4 | ExportFormat::Avi
            if matches!(mode, ExportExecutionMode::SoftwareOnly) =>
        {
            [
                ffmpeg::format::Pixel::YUV420P,
                ffmpeg::format::Pixel::NV12,
                ffmpeg::format::Pixel::YUV422P,
                ffmpeg::format::Pixel::RGB24,
            ]
            .as_slice()
        }
        ExportFormat::Mp4 | ExportFormat::Avi => [
            ffmpeg::format::Pixel::NV12,
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::YUV422P,
            ffmpeg::format::Pixel::RGB24,
        ]
        .as_slice(),
    };

    if let Some(formats) = codec.formats() {
        let available: Vec<_> = formats.collect();
        if let Some(source_pixel) = source_hint
            && available.contains(&source_pixel)
        {
            return source_pixel;
        }
        for pixel in preferred {
            if available.contains(pixel) {
                return *pixel;
            }
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }

    source_hint.unwrap_or(match format {
        ExportFormat::Gif => ffmpeg::format::Pixel::RGB8,
        ExportFormat::Apng => ffmpeg::format::Pixel::RGBA,
        ExportFormat::Webp => ffmpeg::format::Pixel::YUVA420P,
        ExportFormat::Mp4 | ExportFormat::Avi => ffmpeg::format::Pixel::YUV420P,
    })
}

fn choose_audio_codec(format: ExportFormat) -> Option<ffmpeg::Codec> {
    match format {
        ExportFormat::Mp4 => ffmpeg::encoder::find(ffmpeg::codec::Id::AAC),
        ExportFormat::Avi => ffmpeg::encoder::find_by_name("libmp3lame")
            .or_else(|| ffmpeg::encoder::find_by_name("libshine"))
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::MP3))
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::AAC)),
        ExportFormat::Gif | ExportFormat::Apng | ExportFormat::Webp => None,
    }
}

fn choose_audio_sample_rate(codec: ffmpeg::codec::Audio, requested_hz: u32) -> u32 {
    if let Some(rates) = codec.rates() {
        let available: Vec<u32> = rates.map(|rate| rate.max(1) as u32).collect();
        if available.is_empty() {
            return requested_hz.max(1);
        }
        if available.contains(&requested_hz) {
            return requested_hz;
        }
        return available
            .into_iter()
            .min_by_key(|rate| rate.abs_diff(requested_hz))
            .unwrap_or(requested_hz.max(1));
    }
    requested_hz.max(1)
}

fn choose_audio_channel_layout(
    codec: ffmpeg::codec::Audio,
    requested_channels: u16,
) -> ffmpeg::ChannelLayout {
    let requested_layout = ffmpeg::ChannelLayout::default(i32::from(requested_channels.max(1)));
    if let Some(layouts) = codec.channel_layouts() {
        let available: Vec<_> = layouts.collect();
        if available.contains(&requested_layout) {
            return requested_layout;
        }
        if requested_channels >= 2 && available.contains(&ffmpeg::ChannelLayout::STEREO) {
            return ffmpeg::ChannelLayout::STEREO;
        }
        if available.contains(&ffmpeg::ChannelLayout::MONO) {
            return ffmpeg::ChannelLayout::MONO;
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }
    requested_layout
}

fn effective_audio_bitrate_kbps(requested_kbps: u16) -> u16 {
    requested_kbps.clamp(128, 192)
}

fn choose_audio_sample_format(codec: ffmpeg::codec::Audio) -> ffmpeg::format::Sample {
    let preferred = [
        ffmpeg::format::Sample::F32(ffmpeg::format::sample::Type::Planar),
        ffmpeg::format::Sample::F32(ffmpeg::format::sample::Type::Packed),
        ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Planar),
        ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
    ];

    if let Some(formats) = codec.formats() {
        let available: Vec<_> = formats.collect();
        for candidate in preferred {
            if available.contains(&candidate) {
                return candidate;
            }
        }
        if let Some(first) = available.first().copied() {
            return first;
        }
    }

    ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed)
}

fn open_audio_encoder(
    audio_encoder: ffmpeg::codec::encoder::audio::Audio,
    audio_codec: ffmpeg::Codec,
) -> Result<ffmpeg::encoder::audio::Encoder> {
    if audio_codec.name().eq_ignore_ascii_case("aac") {
        return audio_encoder
            .open_as_with(audio_codec, {
                let mut options = ffmpeg::Dictionary::new();
                options.set("profile", "aac_low");
                options
            })
            .map_err(|err| {
                ScreenRecorderError::Export(format!("failed to open audio encoder: {err}"))
            });
    }

    audio_encoder
        .open_as(audio_codec)
        .map_err(|err| ScreenRecorderError::Export(format!("failed to open audio encoder: {err}")))
}

fn export_video_packet_copy_with_generated_audio(
    input_video_path: &Path,
    output_path: &Path,
    format: ExportFormat,
    playback_speed: f32,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    perf_config: &ExportPerformanceConfig,
    cancel_flag: &Arc<AtomicBool>,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<ExportCodecTelemetry> {
    ensure_ffmpeg_initialized()?;
    check_canceled(cancel_flag)?;
    let mut telemetry = ExportCodecTelemetry::default();
    let speed = playback_speed.clamp(0.25, 4.0);
    let adjust_packet_timestamps = (speed - 1.0).abs() > f32::EPSILON;
    let speed_scale = 1.0f64 / f64::from(speed);

    let mut input = ffmpeg::format::input(input_video_path).map_err(|err| {
        ScreenRecorderError::Export(format!(
            "failed to open source video for packet-copy export {}: {err}",
            input_video_path.display()
        ))
    })?;
    let source_video = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "source video {} has no video stream",
                input_video_path.display()
            ))
        })?;
    let source_video_stream_index = source_video.index();
    let source_video_time_base = source_video.time_base();
    let source_video_rate = source_video.rate();
    let source_video_avg_rate = source_video.avg_frame_rate();
    let source_video_frames = source_video.frames().max(0) as usize;
    let source_video_parameters = source_video.parameters();

    let mut output = ffmpeg::format::output(output_path).map_err(|err| {
        ScreenRecorderError::Export(format!("failed to create ffmpeg output context: {err}"))
    })?;
    let global_header = output
        .format()
        .flags()
        .contains(ffmpeg::format::Flags::GLOBAL_HEADER);

    let video_stream_index = {
        let mut stream = output.add_stream(None::<ffmpeg::Codec>).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to add output video stream: {err}"))
        })?;
        stream.set_time_base(source_video_time_base);
        if source_video_rate.numerator() > 0 && source_video_rate.denominator() > 0 {
            stream.set_rate(source_video_rate);
        }
        if source_video_avg_rate.numerator() > 0 && source_video_avg_rate.denominator() > 0 {
            stream.set_avg_frame_rate(source_video_avg_rate);
        }
        stream.set_parameters(source_video_parameters);
        // SAFETY:
        // - `stream` is a valid mutable output stream created by FFmpeg.
        // - Clearing codec_tag is required when remuxing into some containers (especially MP4).
        unsafe {
            (*(*stream.as_mut_ptr()).codecpar).codec_tag = 0;
        }
        stream.index()
    };

    let mut audio_state = None;
    if let Some(mixed) = mixed_audio
        && !format.is_animated_image()
    {
        let container_audio_codec = output
            .format()
            .codec(output_path, ffmpeg::media::Type::Audio);
        let audio_codec = choose_audio_codec(format)
            .or_else(|| ffmpeg::encoder::find(container_audio_codec))
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "no audio encoder available for {format:?} (container={container_audio_codec:?})"
                ))
            })?;
        telemetry.audio_encoder = Some(audio_codec.name().to_string());
        let codec_audio_info = audio_codec.audio().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "selected audio codec is not usable as audio: {err}"
            ))
        })?;
        let output_rate_hz = choose_audio_sample_rate(codec_audio_info, mixed.sample_rate_hz);
        let output_layout = choose_audio_channel_layout(codec_audio_info, mixed.channels);
        let output_sample_format = choose_audio_sample_format(codec_audio_info);

        let mut audio_encoder = ffmpeg::codec::context::Context::new_with_codec(audio_codec)
            .encoder()
            .audio()
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create audio encoder context: {err}"
                ))
            })?;
        audio_encoder.set_rate(output_rate_hz as i32);
        audio_encoder.set_channel_layout(output_layout);
        audio_encoder.set_format(output_sample_format);
        audio_encoder
            .set_bit_rate(usize::from(effective_audio_bitrate_kbps(audio_bitrate_kbps)) * 1000);
        audio_encoder.set_time_base((1, output_rate_hz as i32));
        configure_codec_threads(
            &mut audio_encoder,
            perf_config.encode_threads,
            ffmpeg::codec::threading::Type::Frame,
        );
        if global_header {
            audio_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
        }

        let audio_encoder = open_audio_encoder(audio_encoder, audio_codec)?;

        let audio_stream_index = {
            let mut stream = output.add_stream(audio_codec).map_err(|err| {
                ScreenRecorderError::Export(format!("failed to add output audio stream: {err}"))
            })?;
            stream.set_time_base(ffmpeg::Rational(1, output_rate_hz as i32));
            stream.set_rate(ffmpeg::Rational(output_rate_hz as i32, 1));
            stream.set_parameters(&audio_encoder);
            stream.index()
        };

        audio_state = Some((
            audio_encoder,
            audio_stream_index,
            ffmpeg::Rational(1, output_rate_hz as i32),
        ));
    }

    output.write_header().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output header: {err}"))
    })?;

    let video_stream_time_base = output
        .stream(video_stream_index)
        .map(|stream| stream.time_base())
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "failed to resolve output video stream {} after header",
                video_stream_index
            ))
        })?;

    if let Some((_, stream_index, stream_time_base)) = audio_state.as_mut() {
        *stream_time_base = output
            .stream(*stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "failed to resolve output audio stream {} after header",
                    stream_index
                ))
            })?;
    }

    let audio_worker = if let (Some(audio), Some((audio_encoder, stream_index, stream_time_base))) =
        (mixed_audio.cloned(), audio_state.take())
    {
        Some(spawn_audio_packet_worker(
            audio_encoder,
            stream_index,
            stream_time_base,
            audio,
            Arc::clone(cancel_flag),
        )?)
    } else {
        None
    };
    let mut audio_worker = AudioPacketWorkerGuard::new(audio_worker, Arc::clone(cancel_flag));

    let video_result = (|| -> Result<()> {
        let started = Instant::now();
        let video_copy_start = Instant::now();
        let mut copied_packets = 0usize;
        let mut timestamp_scale_state = PacketTimestampScaleState::default();
        for (stream, mut packet) in input.packets() {
            check_canceled(cancel_flag)?;
            if stream.index() != source_video_stream_index {
                continue;
            }

            packet.set_stream(video_stream_index);
            packet.rescale_ts(source_video_time_base, video_stream_time_base);
            if adjust_packet_timestamps {
                scale_packet_timestamps_for_speed(
                    &mut packet,
                    speed_scale,
                    &mut timestamp_scale_state,
                );
            }
            packet.set_position(-1);
            packet.write_interleaved(&mut output).map_err(|err| {
                ScreenRecorderError::Export(format!("failed to write copied video packet: {err}"))
            })?;
            copied_packets = copied_packets.saturating_add(1);

            if copied_packets.is_multiple_of(32) {
                let elapsed = started.elapsed().as_secs_f32().max(0.001);
                let packets_per_sec = copied_packets as f32 / elapsed;
                let eta_ms = if source_video_frames > copied_packets && packets_per_sec > 0.0 {
                    Some(
                        (((source_video_frames - copied_packets) as f32 / packets_per_sec) * 1000.0)
                            as u64,
                    )
                } else {
                    None
                };
                let percent = if source_video_frames > 0 {
                    35.0 + ((copied_packets as f32 / source_video_frames as f32) * 50.0)
                } else {
                    35.0
                };
                emit_progress(
                    progress_tx,
                    ExportStage::VideoEncode,
                    percent.clamp(35.0, 85.0),
                    packets_per_sec,
                    eta_ms,
                );
            }
        }

        if copied_packets == 0 {
            return Err(ScreenRecorderError::Export(
                "packet-copy export found no video packets in source stream".to_string(),
            ));
        }

        emit_progress(progress_tx, ExportStage::VideoEncode, 85.0, 0.0, Some(0));
        telemetry.stage_durations_ms.video_encode = video_copy_start
            .elapsed()
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
        Ok(())
    })();
    if video_result.is_err() {
        cancel_flag.store(true, Ordering::Release);
    }
    let buffered_audio = if let Some(audio_worker) = audio_worker.take() {
        emit_progress(progress_tx, ExportStage::AudioEncode, 90.0, 0.0, None);
        let audio_encode_start = Instant::now();
        Some((audio_encode_start, join_audio_packet_worker(audio_worker)))
    } else {
        None
    };
    video_result?;
    if let Some((audio_encode_start, buffered_audio)) = buffered_audio {
        let (mut buffered_audio, stream_index, stream_time_base) = buffered_audio?;
        write_buffered_audio_packets(
            &mut output,
            stream_index,
            stream_time_base,
            buffered_audio.encoder_time_base,
            &mut buffered_audio.packets,
        )?;
        telemetry.stage_durations_ms.audio_encode = audio_encode_start
            .elapsed()
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
    }

    emit_progress(progress_tx, ExportStage::Mux, 95.0, 0.0, None);
    let mux_start = Instant::now();
    output.write_trailer().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output trailer: {err}"))
    })?;
    telemetry.stage_durations_ms.mux =
        mux_start.elapsed().as_millis().min(u128::from(u64::MAX)) as u64;
    Ok(telemetry)
}

#[derive(Clone, Copy, Debug)]
struct PacketTimestampScaleState {
    last_pts: Option<i64>,
    last_dts: Option<i64>,
    last_duration: i64,
}

impl Default for PacketTimestampScaleState {
    fn default() -> Self {
        Self {
            last_pts: None,
            last_dts: None,
            last_duration: 1,
        }
    }
}

#[inline]
fn scale_packet_timestamps_for_speed(
    packet: &mut ffmpeg::Packet,
    speed_scale: f64,
    state: &mut PacketTimestampScaleState,
) {
    let scaled_duration =
        scale_packet_duration_ticks(packet.duration(), speed_scale, state.last_duration);
    packet.set_duration(scaled_duration);
    state.last_duration = scaled_duration;

    if let Some(pts) = packet.pts() {
        let mut scaled_pts = scale_packet_tick(pts, speed_scale);
        if let Some(prev) = state.last_pts
            && scaled_pts <= prev
        {
            scaled_pts = prev.saturating_add(scaled_duration.max(1));
        }
        packet.set_pts(Some(scaled_pts));
        state.last_pts = Some(scaled_pts);
    }

    if let Some(dts) = packet.dts() {
        let mut scaled_dts = scale_packet_tick(dts, speed_scale);
        if let Some(prev) = state.last_dts
            && scaled_dts <= prev
        {
            scaled_dts = prev.saturating_add(1);
        }
        packet.set_dts(Some(scaled_dts));
        state.last_dts = Some(scaled_dts);

        if let Some(pts) = packet.pts()
            && pts < scaled_dts
        {
            packet.set_pts(Some(scaled_dts));
            state.last_pts = Some(scaled_dts);
        }
    }
}

#[inline]
fn scale_packet_duration_ticks(duration: i64, speed_scale: f64, fallback: i64) -> i64 {
    let base = if duration > 0 {
        duration
    } else {
        fallback.max(1)
    };
    scale_packet_tick(base, speed_scale).max(1)
}

#[inline]
fn scale_packet_tick(tick: i64, speed_scale: f64) -> i64 {
    ((tick as f64) * speed_scale)
        .round()
        .clamp(i64::MIN as f64, i64::MAX as f64) as i64
}

fn export_video_generated<F>(
    output_path: &Path,
    width: u32,
    height: u32,
    frame_count: usize,
    export_fps: u32,
    format: ExportFormat,
    requested_codec: VideoCodec,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    cancel_flag: &Arc<AtomicBool>,
    progress_tx: &Option<Sender<ExportProgress>>,
    mut rgba_provider: F,
) -> Result<ExportCodecTelemetry>
where
    F: FnMut(usize, &mut [u8]) -> Result<()>,
{
    ensure_ffmpeg_initialized()?;
    validate_export_dimensions(width, height, format.requires_even_dimensions())?;

    let mut output = ffmpeg::format::output(output_path).map_err(|err| {
        ScreenRecorderError::Export(format!("failed to create ffmpeg output context: {err}"))
    })?;
    let global_header = output
        .format()
        .flags()
        .contains(ffmpeg::format::Flags::GLOBAL_HEADER);

    let fps = export_fps.max(1).min(i32::MAX as u32) as i32;
    let video_time_base = ffmpeg::Rational(1, fps);
    let video_frame_rate = ffmpeg::Rational(fps, 1);

    let mut video_codec = select_video_codec(
        &output,
        output_path,
        format,
        requested_codec,
        perf_config.mode,
        perf_config.software_h264_priority,
    )?;
    let codec_video_info = video_codec.video().map_err(|err| {
        ScreenRecorderError::Export(format!(
            "selected video codec is not usable as video: {err}"
        ))
    })?;
    let mut pixel_format =
        choose_video_pixel_format(format, codec_video_info, None, perf_config.mode);
    let mut video_encoder = ffmpeg::codec::context::Context::new_with_codec(video_codec)
        .encoder()
        .video()
        .map_err(|err| {
            ScreenRecorderError::Export(format!("failed to create video encoder context: {err}"))
        })?;
    video_encoder.set_width(width);
    video_encoder.set_height(height);
    video_encoder.set_format(pixel_format);
    video_encoder.set_time_base(video_time_base);
    video_encoder.set_frame_rate(Some(video_frame_rate));
    configure_codec_threads(
        &mut video_encoder,
        perf_config.encode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );

    let effective_video = effective_video_config(video_config);
    if !format.is_animated_image() {
        video_encoder.set_bit_rate(smart_quality_bitrate_bps(
            width,
            height,
            export_fps,
            &effective_video,
            false,
        ));
    }
    if global_header {
        video_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }

    let mut video_encoder = match open_video_encoder(video_encoder, &video_codec, &effective_video)
    {
        Ok(encoder) => encoder,
        Err(primary_error) => {
            // If hardware auto-select picked an encoder that fails to open,
            // transparently fall back to software H.264.
            if hardware_video_encode_allowed(perf_config.mode)
                && is_hardware_video_encoder(&video_codec)
            {
                video_codec = select_video_codec(
                    &output,
                    output_path,
                    format,
                    requested_codec,
                    ExportExecutionMode::SoftwareOnly,
                    perf_config.software_h264_priority,
                )?;
                let fallback_video_info = video_codec.video().map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "fallback software codec is not usable as video: {err}"
                    ))
                })?;
                pixel_format = choose_video_pixel_format(
                    format,
                    fallback_video_info,
                    None,
                    ExportExecutionMode::SoftwareOnly,
                );

                let mut fallback_encoder =
                    ffmpeg::codec::context::Context::new_with_codec(video_codec)
                        .encoder()
                        .video()
                        .map_err(|err| {
                            ScreenRecorderError::Export(format!(
                                "failed to create fallback video encoder context: {err}"
                            ))
                        })?;
                fallback_encoder.set_width(width);
                fallback_encoder.set_height(height);
                fallback_encoder.set_format(pixel_format);
                fallback_encoder.set_time_base(video_time_base);
                fallback_encoder.set_frame_rate(Some(video_frame_rate));
                configure_codec_threads(
                    &mut fallback_encoder,
                    perf_config.encode_threads,
                    ffmpeg::codec::threading::Type::Frame,
                );
                if !format.is_animated_image() {
                    fallback_encoder.set_bit_rate(smart_quality_bitrate_bps(
                        width,
                        height,
                        export_fps,
                        &effective_video,
                        false,
                    ));
                }
                if global_header {
                    fallback_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
                }

                open_video_encoder(fallback_encoder, &video_codec, &effective_video)
                    .map_err(|_| primary_error)?
            } else {
                return Err(primary_error);
            }
        }
    };

    let video_stream_index = {
        let mut stream = output.add_stream(video_codec).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to add output video stream: {err}"))
        })?;
        stream.set_time_base(video_time_base);
        stream.set_rate(video_frame_rate);
        stream.set_avg_frame_rate(video_frame_rate);
        stream.set_parameters(&video_encoder);
        stream.index()
    };
    let mut telemetry = ExportCodecTelemetry {
        video_encoder: Some(video_codec.name().to_string()),
        used_hardware_encode: is_hardware_h264_encoder(&video_codec),
        ..ExportCodecTelemetry::default()
    };

    let mut audio_state = None;
    if let Some(mixed) = mixed_audio
        && !format.is_animated_image()
    {
        let container_audio_codec = output
            .format()
            .codec(output_path, ffmpeg::media::Type::Audio);
        let audio_codec = choose_audio_codec(format)
            .or_else(|| ffmpeg::encoder::find(container_audio_codec))
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "no audio encoder available for {format:?} (container={container_audio_codec:?})"
                ))
            })?;
        telemetry.audio_encoder = Some(audio_codec.name().to_string());
        let codec_audio_info = audio_codec.audio().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "selected audio codec is not usable as audio: {err}"
            ))
        })?;
        let output_rate_hz = choose_audio_sample_rate(codec_audio_info, mixed.sample_rate_hz);
        let output_layout = choose_audio_channel_layout(codec_audio_info, mixed.channels);
        let output_sample_format = choose_audio_sample_format(codec_audio_info);

        let mut audio_encoder = ffmpeg::codec::context::Context::new_with_codec(audio_codec)
            .encoder()
            .audio()
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create audio encoder context: {err}"
                ))
            })?;
        audio_encoder.set_rate(output_rate_hz as i32);
        audio_encoder.set_channel_layout(output_layout);
        audio_encoder.set_format(output_sample_format);
        audio_encoder
            .set_bit_rate(usize::from(effective_audio_bitrate_kbps(audio_bitrate_kbps)) * 1000);
        audio_encoder.set_time_base((1, output_rate_hz as i32));
        configure_codec_threads(
            &mut audio_encoder,
            perf_config.encode_threads,
            ffmpeg::codec::threading::Type::Frame,
        );
        if global_header {
            audio_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
        }

        let audio_encoder = open_audio_encoder(audio_encoder, audio_codec)?;

        let audio_stream_index = {
            let mut stream = output.add_stream(audio_codec).map_err(|err| {
                ScreenRecorderError::Export(format!("failed to add output audio stream: {err}"))
            })?;
            stream.set_time_base(ffmpeg::Rational(1, output_rate_hz as i32));
            stream.set_rate(ffmpeg::Rational(output_rate_hz as i32, 1));
            stream.set_parameters(&audio_encoder);
            stream.index()
        };

        audio_state = Some((
            audio_encoder,
            audio_stream_index,
            ffmpeg::Rational(1, output_rate_hz as i32),
        ));
    }

    output.write_header().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output header: {err}"))
    })?;

    let video_stream_time_base = output
        .stream(video_stream_index)
        .map(|stream| stream.time_base())
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "failed to resolve output video stream {} after header",
                video_stream_index
            ))
        })?;

    if let Some((_, stream_index, stream_time_base)) = audio_state.as_mut() {
        *stream_time_base = output
            .stream(*stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "failed to resolve output audio stream {} after header",
                    stream_index
                ))
            })?;
    }

    let mut scaler = ffmpeg::software::scaling::Context::get(
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
    let mut generated_rgba = if can_write_direct_rgba {
        Vec::new()
    } else {
        vec![0u8; rgba_len]
    };
    let start = Instant::now();
    let video_encode_start = Instant::now();
    let encoded_output_count = Cell::new(0usize);
    let scheduled_output_count = Cell::new(0usize);
    let mut pending_video_packet_durations = VecDeque::new();
    let mut pending_collapsed_video_frame = PendingCollapsedVideoFrame::default();

    for index in 0..frame_count {
        check_canceled(cancel_flag)?;
        if can_write_direct_rgba {
            {
                let plane = rgba_frame.data_mut(0);
                rgba_provider(index, &mut plane[..rgba_len])?;
            }
        } else {
            rgba_provider(index, &mut generated_rgba)?;
            copy_rgba_into_frame(&mut rgba_frame, width, &generated_rgba);
        }
        ensure_video_frame_writable(&mut encode_frame)?;
        scaler.run(&rgba_frame, &mut encode_frame).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to convert frame for video export: {err}"))
        })?;

        queue_video_frame_with_repeat_collapse(
            &mut video_encoder,
            &mut output,
            video_stream_index,
            video_stream_time_base,
            &mut encode_frame,
            1,
            true,
            &mut pending_collapsed_video_frame,
            &mut pending_video_packet_durations,
            &encoded_output_count,
            &scheduled_output_count,
            frame_count,
            &start,
            progress_tx,
        )?;

        if index % 10 == 0 || index + 1 == frame_count {
            let elapsed = start.elapsed().as_secs_f32().max(0.001);
            let fps = (index + 1) as f32 / elapsed;
            let remaining_frames = frame_count.saturating_sub(index + 1) as f32;
            let eta_ms = if fps > 0.0 {
                Some(((remaining_frames / fps) * 1000.0) as u64)
            } else {
                None
            };
            let percent = 35.0 + (((index + 1) as f32 / frame_count.max(1) as f32) * 55.0);
            emit_progress(progress_tx, ExportStage::VideoEncode, percent, fps, eta_ms);
        }
    }

    flush_pending_collapsed_video_frame(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        &mut pending_collapsed_video_frame,
        &mut pending_video_packet_durations,
        &encoded_output_count,
        frame_count,
        &start,
        progress_tx,
    )?;

    video_encoder.send_eof().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to finalize video encoder: {err}"))
    })?;
    drain_video_packets_with_durations(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        true,
        &mut pending_video_packet_durations,
    )?;
    telemetry.stage_durations_ms.video_encode = video_encode_start
        .elapsed()
        .as_millis()
        .min(u128::from(u64::MAX)) as u64;

    if let (Some(audio), Some((audio_encoder, stream_index, stream_time_base))) =
        (mixed_audio, audio_state.as_mut())
    {
        emit_progress(progress_tx, ExportStage::AudioEncode, 90.0, 0.0, None);
        let audio_encode_start = Instant::now();
        encode_audio_samples(
            &mut output,
            audio_encoder,
            *stream_index,
            *stream_time_base,
            audio,
        )?;
        telemetry.stage_durations_ms.audio_encode = audio_encode_start
            .elapsed()
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
    }

    emit_progress(progress_tx, ExportStage::Mux, 95.0, 0.0, None);
    let mux_start = Instant::now();
    output.write_trailer().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output trailer: {err}"))
    })?;
    telemetry.stage_durations_ms.mux =
        mux_start.elapsed().as_millis().min(u128::from(u64::MAX)) as u64;
    Ok(telemetry)
}

fn build_source_frame_repeat_counts(retime: &RetimePlan) -> Vec<usize> {
    let Some(&max_source_index) = retime.source_indices.last() else {
        return Vec::new();
    };
    let mut repeat_counts = vec![0usize; max_source_index.saturating_add(1)];
    for &source_index in &retime.source_indices {
        if let Some(slot) = repeat_counts.get_mut(source_index) {
            *slot = slot.saturating_add(1);
        }
    }
    repeat_counts
}

fn export_video_generated_from_source(
    input_video_path: &Path,
    output_path: &Path,
    retime: &RetimePlan,
    width: u32,
    height: u32,
    export_fps: u32,
    format: ExportFormat,
    requested_codec: VideoCodec,
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    cancel_flag: &Arc<AtomicBool>,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<ExportCodecTelemetry> {
    ensure_ffmpeg_initialized()?;
    validate_export_dimensions(width, height, format.requires_even_dimensions())?;

    if retime.frame_count == 0 {
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
    let (mut decoder, hw_decode_state) =
        open_source_video_decoder(&input_video_parameters, perf_config, true)?;

    let mut output = ffmpeg::format::output(output_path).map_err(|err| {
        ScreenRecorderError::Export(format!("failed to create ffmpeg output context: {err}"))
    })?;
    let global_header = output
        .format()
        .flags()
        .contains(ffmpeg::format::Flags::GLOBAL_HEADER);

    let fps = export_fps.max(1).min(i32::MAX as u32) as i32;
    let video_time_base = ffmpeg::Rational(1, fps);
    let video_frame_rate = ffmpeg::Rational(fps, 1);

    let mut video_codec = select_video_codec(
        &output,
        output_path,
        format,
        requested_codec,
        perf_config.mode,
        perf_config.software_h264_priority,
    )?;
    let codec_video_info = video_codec.video().map_err(|err| {
        ScreenRecorderError::Export(format!(
            "selected video codec is not usable as video: {err}"
        ))
    })?;
    let decoder_output_format = decoder_software_output_format(&decoder, hw_decode_state.as_ref());
    let mut pixel_format = choose_video_pixel_format(
        format,
        codec_video_info,
        Some(decoder_output_format),
        perf_config.mode,
    );
    let mut video_encoder = ffmpeg::codec::context::Context::new_with_codec(video_codec)
        .encoder()
        .video()
        .map_err(|err| {
            ScreenRecorderError::Export(format!("failed to create video encoder context: {err}"))
        })?;
    video_encoder.set_width(width);
    video_encoder.set_height(height);
    video_encoder.set_format(pixel_format);
    video_encoder.set_time_base(video_time_base);
    video_encoder.set_frame_rate(Some(video_frame_rate));
    configure_codec_threads(
        &mut video_encoder,
        perf_config.encode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );

    let effective_video = effective_video_config(video_config);
    if !format.is_animated_image() {
        video_encoder.set_bit_rate(smart_quality_bitrate_bps(
            width,
            height,
            export_fps,
            &effective_video,
            false,
        ));
    }
    if global_header {
        video_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }

    let mut video_encoder = match open_video_encoder(video_encoder, &video_codec, &effective_video)
    {
        Ok(encoder) => encoder,
        Err(primary_error) => {
            if hardware_video_encode_allowed(perf_config.mode)
                && is_hardware_video_encoder(&video_codec)
            {
                video_codec = select_video_codec(
                    &output,
                    output_path,
                    format,
                    requested_codec,
                    ExportExecutionMode::SoftwareOnly,
                    perf_config.software_h264_priority,
                )?;
                let fallback_video_info = video_codec.video().map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "fallback software codec is not usable as video: {err}"
                    ))
                })?;
                pixel_format = choose_video_pixel_format(
                    format,
                    fallback_video_info,
                    Some(decoder_output_format),
                    ExportExecutionMode::SoftwareOnly,
                );

                let mut fallback_encoder =
                    ffmpeg::codec::context::Context::new_with_codec(video_codec)
                        .encoder()
                        .video()
                        .map_err(|err| {
                            ScreenRecorderError::Export(format!(
                                "failed to create fallback video encoder context: {err}"
                            ))
                        })?;
                fallback_encoder.set_width(width);
                fallback_encoder.set_height(height);
                fallback_encoder.set_format(pixel_format);
                fallback_encoder.set_time_base(video_time_base);
                fallback_encoder.set_frame_rate(Some(video_frame_rate));
                configure_codec_threads(
                    &mut fallback_encoder,
                    perf_config.encode_threads,
                    ffmpeg::codec::threading::Type::Frame,
                );
                if !format.is_animated_image() {
                    fallback_encoder.set_bit_rate(smart_quality_bitrate_bps(
                        width,
                        height,
                        export_fps,
                        &effective_video,
                        false,
                    ));
                }
                if global_header {
                    fallback_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
                }

                open_video_encoder(fallback_encoder, &video_codec, &effective_video)
                    .map_err(|_| primary_error)?
            } else {
                return Err(primary_error);
            }
        }
    };

    let video_stream_index = {
        let mut stream = output.add_stream(video_codec).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to add output video stream: {err}"))
        })?;
        stream.set_time_base(video_time_base);
        stream.set_rate(video_frame_rate);
        stream.set_avg_frame_rate(video_frame_rate);
        stream.set_parameters(&video_encoder);
        stream.index()
    };
    let mut telemetry = ExportCodecTelemetry {
        video_decoder: Some(
            hw_decode_state
                .as_ref()
                .map(|state| state.device_name)
                .unwrap_or("software_decode")
                .to_string(),
        ),
        video_encoder: Some(video_codec.name().to_string()),
        used_hardware_decode: hw_decode_state.is_some(),
        used_hardware_encode: is_hardware_h264_encoder(&video_codec),
        ..ExportCodecTelemetry::default()
    };

    let mut audio_state = None;
    if let Some(mixed) = mixed_audio
        && !format.is_animated_image()
    {
        let container_audio_codec = output
            .format()
            .codec(output_path, ffmpeg::media::Type::Audio);
        let audio_codec = choose_audio_codec(format)
            .or_else(|| ffmpeg::encoder::find(container_audio_codec))
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "no audio encoder available for {format:?} (container={container_audio_codec:?})"
                ))
            })?;
        telemetry.audio_encoder = Some(audio_codec.name().to_string());
        let codec_audio_info = audio_codec.audio().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "selected audio codec is not usable as audio: {err}"
            ))
        })?;
        let output_rate_hz = choose_audio_sample_rate(codec_audio_info, mixed.sample_rate_hz);
        let output_layout = choose_audio_channel_layout(codec_audio_info, mixed.channels);
        let output_sample_format = choose_audio_sample_format(codec_audio_info);

        let mut audio_encoder = ffmpeg::codec::context::Context::new_with_codec(audio_codec)
            .encoder()
            .audio()
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create audio encoder context: {err}"
                ))
            })?;
        audio_encoder.set_rate(output_rate_hz as i32);
        audio_encoder.set_channel_layout(output_layout);
        audio_encoder.set_format(output_sample_format);
        audio_encoder
            .set_bit_rate(usize::from(effective_audio_bitrate_kbps(audio_bitrate_kbps)) * 1000);
        audio_encoder.set_time_base((1, output_rate_hz as i32));
        configure_codec_threads(
            &mut audio_encoder,
            perf_config.encode_threads,
            ffmpeg::codec::threading::Type::Frame,
        );
        if global_header {
            audio_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
        }

        let audio_encoder = open_audio_encoder(audio_encoder, audio_codec)?;

        let audio_stream_index = {
            let mut stream = output.add_stream(audio_codec).map_err(|err| {
                ScreenRecorderError::Export(format!("failed to add output audio stream: {err}"))
            })?;
            stream.set_time_base(ffmpeg::Rational(1, output_rate_hz as i32));
            stream.set_rate(ffmpeg::Rational(output_rate_hz as i32, 1));
            stream.set_parameters(&audio_encoder);
            stream.index()
        };

        audio_state = Some((
            audio_encoder,
            audio_stream_index,
            ffmpeg::Rational(1, output_rate_hz as i32),
        ));
    }

    output.write_header().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output header: {err}"))
    })?;

    let video_stream_time_base = output
        .stream(video_stream_index)
        .map(|stream| stream.time_base())
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "failed to resolve output video stream {} after header",
                video_stream_index
            ))
        })?;

    if let Some((_, stream_index, stream_time_base)) = audio_state.as_mut() {
        *stream_time_base = output
            .stream(*stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "failed to resolve output audio stream {} after header",
                    stream_index
                ))
            })?;
    }

    let audio_worker = if let (Some(audio), Some((audio_encoder, stream_index, stream_time_base))) =
        (mixed_audio.cloned(), audio_state.take())
    {
        Some(spawn_audio_packet_worker(
            audio_encoder,
            stream_index,
            stream_time_base,
            audio,
            Arc::clone(cancel_flag),
        )?)
    } else {
        None
    };
    let mut audio_worker = AudioPacketWorkerGuard::new(audio_worker, Arc::clone(cancel_flag));

    let mut scaler = None::<ffmpeg::software::scaling::Context>;
    let mut encode_frame = None::<ffmpeg::frame::Video>;
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut transferred_decoded = ffmpeg::frame::Video::empty();
    let mut source_frame_index = 0usize;
    let encoded_output_count = Cell::new(0usize);
    let scheduled_output_count = Cell::new(0usize);
    let mut decode_complete = false;
    let start = Instant::now();
    let video_encode_start = Instant::now();
    let mut pending_video_packet_durations = VecDeque::new();
    let mut pending_collapsed_video_frame = PendingCollapsedVideoFrame::default();

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
        if direct_frame_passthrough {
            queue_video_frame_with_repeat_collapse(
                &mut video_encoder,
                &mut output,
                video_stream_index,
                video_stream_time_base,
                decoded,
                repeats,
                true,
                &mut pending_collapsed_video_frame,
                &mut pending_video_packet_durations,
                &encoded_output_count,
                &scheduled_output_count,
                retime.frame_count,
                &start,
                progress_tx,
            )?;
            return Ok(());
        }

        let needs_reset = scaler.is_none()
            || encode_frame
                .as_ref()
                .map(|frame| frame.width() != width || frame.height() != height)
                .unwrap_or(false);
        if needs_reset {
            scaler = Some(
                ffmpeg::software::scaling::Context::get(
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

        queue_video_frame_with_repeat_collapse(
            &mut video_encoder,
            &mut output,
            video_stream_index,
            video_stream_time_base,
            encode_frame_ref,
            repeats,
            true,
            &mut pending_collapsed_video_frame,
            &mut pending_video_packet_durations,
            &encoded_output_count,
            &scheduled_output_count,
            retime.frame_count,
            &start,
            progress_tx,
        )?;
        Ok(())
    };

    for (stream, packet) in input.packets() {
        if decode_complete || scheduled_output_count.get() >= retime.frame_count {
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
                    if scheduled_output_count.get() >= retime.frame_count {
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
                    if scheduled_output_count.get() >= retime.frame_count {
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

    if scheduled_output_count.get() != retime.frame_count {
        return Err(ScreenRecorderError::Export(format!(
            "source decode ended before all retimed frames were produced (scheduled {}, expected {})",
            scheduled_output_count.get(),
            retime.frame_count
        )));
    }

    flush_pending_collapsed_video_frame(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        &mut pending_collapsed_video_frame,
        &mut pending_video_packet_durations,
        &encoded_output_count,
        retime.frame_count,
        &start,
        progress_tx,
    )?;

    if encoded_output_count.get() != retime.frame_count {
        return Err(ScreenRecorderError::Export(format!(
            "source encode ended before all retimed frames were produced (encoded {}, expected {})",
            encoded_output_count.get(),
            retime.frame_count
        )));
    }

    video_encoder.send_eof().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to finalize video encoder: {err}"))
    })?;
    drain_video_packets_with_durations(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        true,
        &mut pending_video_packet_durations,
    )?;
    telemetry.stage_durations_ms.video_encode = video_encode_start
        .elapsed()
        .as_millis()
        .min(u128::from(u64::MAX)) as u64;

    if let Some(audio_worker) = audio_worker.take() {
        emit_progress(progress_tx, ExportStage::AudioEncode, 90.0, 0.0, None);
        let audio_encode_start = Instant::now();
        let (mut buffered_audio, stream_index, stream_time_base) =
            join_audio_packet_worker(audio_worker)?;
        write_buffered_audio_packets(
            &mut output,
            stream_index,
            stream_time_base,
            buffered_audio.encoder_time_base,
            &mut buffered_audio.packets,
        )?;
        telemetry.stage_durations_ms.audio_encode = audio_encode_start
            .elapsed()
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
    }

    emit_progress(progress_tx, ExportStage::Mux, 95.0, 0.0, None);
    let mux_start = Instant::now();
    output.write_trailer().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output trailer: {err}"))
    })?;
    telemetry.stage_durations_ms.mux =
        mux_start.elapsed().as_millis().min(u128::from(u64::MAX)) as u64;
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
    mixed_audio: Option<&AudioMixPlan>,
    audio_bitrate_kbps: u16,
    video_config: &VideoEncodeConfig,
    perf_config: &ExportPerformanceConfig,
    overlay_tracks: &MouseTracks,
    mouse_config: &MouseEditConfig,
    cancel_flag: &Arc<AtomicBool>,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<ExportCodecTelemetry> {
    ensure_ffmpeg_initialized()?;
    validate_export_dimensions(width, height, format.requires_even_dimensions())?;

    if retime.frame_count == 0 {
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
    let (mut decoder, hw_decode_state) =
        open_source_video_decoder(&input_video_parameters, perf_config, false)?;

    let mut output = ffmpeg::format::output(output_path).map_err(|err| {
        ScreenRecorderError::Export(format!("failed to create ffmpeg output context: {err}"))
    })?;
    let global_header = output
        .format()
        .flags()
        .contains(ffmpeg::format::Flags::GLOBAL_HEADER);

    let fps = export_fps.max(1).min(i32::MAX as u32) as i32;
    let video_time_base = ffmpeg::Rational(1, fps);
    let video_frame_rate = ffmpeg::Rational(fps, 1);

    let mut video_codec = select_video_codec(
        &output,
        output_path,
        format,
        requested_codec,
        perf_config.mode,
        perf_config.software_h264_priority,
    )?;
    let codec_video_info = video_codec.video().map_err(|err| {
        ScreenRecorderError::Export(format!(
            "selected video codec is not usable as video: {err}"
        ))
    })?;
    let mut pixel_format =
        choose_video_pixel_format(format, codec_video_info, None, perf_config.mode);
    let mut video_encoder = ffmpeg::codec::context::Context::new_with_codec(video_codec)
        .encoder()
        .video()
        .map_err(|err| {
            ScreenRecorderError::Export(format!("failed to create video encoder context: {err}"))
        })?;
    video_encoder.set_width(width);
    video_encoder.set_height(height);
    video_encoder.set_format(pixel_format);
    video_encoder.set_time_base(video_time_base);
    video_encoder.set_frame_rate(Some(video_frame_rate));
    configure_codec_threads(
        &mut video_encoder,
        perf_config.encode_threads,
        ffmpeg::codec::threading::Type::Frame,
    );

    let effective_video = effective_video_config(video_config);
    if !format.is_animated_image() {
        video_encoder.set_bit_rate(smart_quality_bitrate_bps(
            width,
            height,
            export_fps,
            &effective_video,
            false,
        ));
    }
    if global_header {
        video_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }

    let mut video_encoder = match open_video_encoder(video_encoder, &video_codec, &effective_video)
    {
        Ok(encoder) => encoder,
        Err(primary_error) => {
            if hardware_video_encode_allowed(perf_config.mode)
                && is_hardware_video_encoder(&video_codec)
            {
                video_codec = select_video_codec(
                    &output,
                    output_path,
                    format,
                    requested_codec,
                    ExportExecutionMode::SoftwareOnly,
                    perf_config.software_h264_priority,
                )?;
                let fallback_video_info = video_codec.video().map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "fallback software codec is not usable as video: {err}"
                    ))
                })?;
                pixel_format = choose_video_pixel_format(
                    format,
                    fallback_video_info,
                    None,
                    ExportExecutionMode::SoftwareOnly,
                );

                let mut fallback_encoder =
                    ffmpeg::codec::context::Context::new_with_codec(video_codec)
                        .encoder()
                        .video()
                        .map_err(|err| {
                            ScreenRecorderError::Export(format!(
                                "failed to create fallback video encoder context: {err}"
                            ))
                        })?;
                fallback_encoder.set_width(width);
                fallback_encoder.set_height(height);
                fallback_encoder.set_format(pixel_format);
                fallback_encoder.set_time_base(video_time_base);
                fallback_encoder.set_frame_rate(Some(video_frame_rate));
                configure_codec_threads(
                    &mut fallback_encoder,
                    perf_config.encode_threads,
                    ffmpeg::codec::threading::Type::Frame,
                );
                if !format.is_animated_image() {
                    fallback_encoder.set_bit_rate(smart_quality_bitrate_bps(
                        width,
                        height,
                        export_fps,
                        &effective_video,
                        false,
                    ));
                }
                if global_header {
                    fallback_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
                }

                open_video_encoder(fallback_encoder, &video_codec, &effective_video)
                    .map_err(|_| primary_error)?
            } else {
                return Err(primary_error);
            }
        }
    };

    let video_stream_index = {
        let mut stream = output.add_stream(video_codec).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to add output video stream: {err}"))
        })?;
        stream.set_time_base(video_time_base);
        stream.set_rate(video_frame_rate);
        stream.set_avg_frame_rate(video_frame_rate);
        stream.set_parameters(&video_encoder);
        stream.index()
    };
    let mut telemetry = ExportCodecTelemetry {
        video_decoder: Some(
            hw_decode_state
                .as_ref()
                .map(|state| state.device_name)
                .unwrap_or("software_decode")
                .to_string(),
        ),
        video_encoder: Some(video_codec.name().to_string()),
        used_hardware_decode: hw_decode_state.is_some(),
        used_hardware_encode: is_hardware_h264_encoder(&video_codec),
        ..ExportCodecTelemetry::default()
    };

    let mut audio_state = None;
    if let Some(mixed) = mixed_audio
        && !format.is_animated_image()
    {
        let container_audio_codec = output
            .format()
            .codec(output_path, ffmpeg::media::Type::Audio);
        let audio_codec = choose_audio_codec(format)
            .or_else(|| ffmpeg::encoder::find(container_audio_codec))
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "no audio encoder available for {format:?} (container={container_audio_codec:?})"
                ))
            })?;
        telemetry.audio_encoder = Some(audio_codec.name().to_string());
        let codec_audio_info = audio_codec.audio().map_err(|err| {
            ScreenRecorderError::Export(format!(
                "selected audio codec is not usable as audio: {err}"
            ))
        })?;
        let output_rate_hz = choose_audio_sample_rate(codec_audio_info, mixed.sample_rate_hz);
        let output_layout = choose_audio_channel_layout(codec_audio_info, mixed.channels);
        let output_sample_format = choose_audio_sample_format(codec_audio_info);

        let mut audio_encoder = ffmpeg::codec::context::Context::new_with_codec(audio_codec)
            .encoder()
            .audio()
            .map_err(|err| {
                ScreenRecorderError::Export(format!(
                    "failed to create audio encoder context: {err}"
                ))
            })?;
        audio_encoder.set_rate(output_rate_hz as i32);
        audio_encoder.set_channel_layout(output_layout);
        audio_encoder.set_format(output_sample_format);
        audio_encoder
            .set_bit_rate(usize::from(effective_audio_bitrate_kbps(audio_bitrate_kbps)) * 1000);
        audio_encoder.set_time_base((1, output_rate_hz as i32));
        configure_codec_threads(
            &mut audio_encoder,
            perf_config.encode_threads,
            ffmpeg::codec::threading::Type::Frame,
        );
        if global_header {
            audio_encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
        }

        let audio_encoder = open_audio_encoder(audio_encoder, audio_codec)?;

        let stream_index = {
            let mut stream = output.add_stream(audio_codec).map_err(|err| {
                ScreenRecorderError::Export(format!("failed to add output audio stream: {err}"))
            })?;
            stream.set_time_base((1, output_rate_hz as i32));
            stream.set_rate((output_rate_hz as i32, 1));
            stream.set_parameters(&audio_encoder);
            stream.index()
        };
        audio_state = Some((audio_encoder, stream_index, ffmpeg::Rational(0, 1)));
    }

    output.write_header().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output header: {err}"))
    })?;

    let video_stream_time_base = output
        .stream(video_stream_index)
        .map(|stream| stream.time_base())
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "failed to resolve output video stream {} after header",
                video_stream_index
            ))
        })?;

    if let Some((_, stream_index, stream_time_base)) = audio_state.as_mut() {
        *stream_time_base = output
            .stream(*stream_index)
            .map(|stream| stream.time_base())
            .ok_or_else(|| {
                ScreenRecorderError::Export(format!(
                    "failed to resolve output audio stream {} after header",
                    stream_index
                ))
            })?;
    }

    let audio_worker = if let (Some(audio), Some((audio_encoder, stream_index, stream_time_base))) =
        (mixed_audio.cloned(), audio_state.take())
    {
        Some(spawn_audio_packet_worker(
            audio_encoder,
            stream_index,
            stream_time_base,
            audio,
            Arc::clone(cancel_flag),
        )?)
    } else {
        None
    };
    let mut audio_worker = AudioPacketWorkerGuard::new(audio_worker, Arc::clone(cancel_flag));

    let mut encode_scaler = ffmpeg::software::scaling::Context::get(
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
    let mut source_encode_scaler = None::<ffmpeg::software::scaling::Context>;
    let mut source_encode_frame = None::<ffmpeg::frame::Video>;
    let mut source_encode_key = None::<(u32, u32, ffmpeg::format::Pixel)>;
    let mut native_overlay_frame = None::<ffmpeg::frame::Video>;
    let mut native_overlay_key = None::<(u32, u32, ffmpeg::format::Pixel)>;
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut transferred_decoded = ffmpeg::frame::Video::empty();
    let mut source_frame_index = 0usize;
    let encoded_output_count = Cell::new(0usize);
    let scheduled_output_count = Cell::new(0usize);
    let mut decode_complete = false;
    let start = Instant::now();
    let video_encode_start = Instant::now();
    let mut pending_video_packet_durations = VecDeque::new();
    let mut pending_collapsed_video_frame = PendingCollapsedVideoFrame::default();

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
            let output_ts = retime.output_timestamps_ms[output_index];
            let decision =
                advance_overlay_state(output_ts, overlay_tracks, mouse_config, &mut overlay_state);
            let repeat_key = repeatable_overlay_frame_key(decision);
            let allow_repeat_collapse = repeat_key.is_some();
            let mut frame_duration_ticks = 1usize;
            if let Some(repeat_key) = repeat_key {
                let mut scan_state = overlay_state.clone();
                while repeat_index + frame_duration_ticks < repeats {
                    let next_output_index = output_index + frame_duration_ticks;
                    let next_output_ts = retime.output_timestamps_ms[next_output_index];
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
                        queue_video_frame_with_repeat_collapse(
                            &mut video_encoder,
                            &mut output,
                            video_stream_index,
                            video_stream_time_base,
                            native_frame,
                            frame_duration_ticks,
                            allow_repeat_collapse,
                            &mut pending_collapsed_video_frame,
                            &mut pending_video_packet_durations,
                            &encoded_output_count,
                            &scheduled_output_count,
                            retime.frame_count,
                            &start,
                            progress_tx,
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
                        queue_video_frame_with_repeat_collapse(
                            &mut video_encoder,
                            &mut output,
                            video_stream_index,
                            video_stream_time_base,
                            direct_frame,
                            frame_duration_ticks,
                            allow_repeat_collapse,
                            &mut pending_collapsed_video_frame,
                            &mut pending_video_packet_durations,
                            &encoded_output_count,
                            &scheduled_output_count,
                            retime.frame_count,
                            &start,
                            progress_tx,
                        )?;
                    }
                    repeat_index += frame_duration_ticks;
                    continue;
                }
            }

            if !decision.needs_draw() {
                if direct_frame_passthrough {
                    queue_video_frame_with_repeat_collapse(
                        &mut video_encoder,
                        &mut output,
                        video_stream_index,
                        video_stream_time_base,
                        decoded,
                        frame_duration_ticks,
                        allow_repeat_collapse,
                        &mut pending_collapsed_video_frame,
                        &mut pending_video_packet_durations,
                        &encoded_output_count,
                        &scheduled_output_count,
                        retime.frame_count,
                        &start,
                        progress_tx,
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
                    queue_video_frame_with_repeat_collapse(
                        &mut video_encoder,
                        &mut output,
                        video_stream_index,
                        video_stream_time_base,
                        direct_frame,
                        frame_duration_ticks,
                        allow_repeat_collapse,
                        &mut pending_collapsed_video_frame,
                        &mut pending_video_packet_durations,
                        &encoded_output_count,
                        &scheduled_output_count,
                        retime.frame_count,
                        &start,
                        progress_tx,
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
                queue_video_frame_with_repeat_collapse(
                    &mut video_encoder,
                    &mut output,
                    video_stream_index,
                    video_stream_time_base,
                    &mut encode_frame,
                    1,
                    allow_repeat_collapse,
                    &mut pending_collapsed_video_frame,
                    &mut pending_video_packet_durations,
                    &encoded_output_count,
                    &scheduled_output_count,
                    retime.frame_count,
                    &start,
                    progress_tx,
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
            queue_video_frame_with_repeat_collapse(
                &mut video_encoder,
                &mut output,
                video_stream_index,
                video_stream_time_base,
                &mut encode_frame,
                frame_duration_ticks,
                allow_repeat_collapse,
                &mut pending_collapsed_video_frame,
                &mut pending_video_packet_durations,
                &encoded_output_count,
                &scheduled_output_count,
                retime.frame_count,
                &start,
                progress_tx,
            )?;
            repeat_index += frame_duration_ticks;
        }
        Ok(())
    };

    for (stream, packet) in input.packets() {
        if decode_complete || scheduled_output_count.get() >= retime.frame_count {
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
                    if scheduled_output_count.get() >= retime.frame_count {
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
                    if scheduled_output_count.get() >= retime.frame_count {
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

    if scheduled_output_count.get() != retime.frame_count {
        return Err(ScreenRecorderError::Export(format!(
            "source decode ended before all overlay frames were produced (scheduled {}, expected {})",
            scheduled_output_count.get(),
            retime.frame_count
        )));
    }

    flush_pending_collapsed_video_frame(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        &mut pending_collapsed_video_frame,
        &mut pending_video_packet_durations,
        &encoded_output_count,
        retime.frame_count,
        &start,
        progress_tx,
    )?;

    if encoded_output_count.get() != retime.frame_count {
        return Err(ScreenRecorderError::Export(format!(
            "source encode ended before all overlay frames were produced (encoded {}, expected {})",
            encoded_output_count.get(),
            retime.frame_count
        )));
    }

    video_encoder.send_eof().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to finalize video encoder: {err}"))
    })?;
    drain_video_packets_with_durations(
        &mut video_encoder,
        &mut output,
        video_stream_index,
        video_stream_time_base,
        true,
        &mut pending_video_packet_durations,
    )?;
    telemetry.stage_durations_ms.video_encode = video_encode_start
        .elapsed()
        .as_millis()
        .min(u128::from(u64::MAX)) as u64;

    if let Some(audio_worker) = audio_worker.take() {
        emit_progress(progress_tx, ExportStage::AudioEncode, 90.0, 0.0, None);
        let audio_encode_start = Instant::now();
        let (mut buffered_audio, stream_index, stream_time_base) =
            join_audio_packet_worker(audio_worker)?;
        write_buffered_audio_packets(
            &mut output,
            stream_index,
            stream_time_base,
            buffered_audio.encoder_time_base,
            &mut buffered_audio.packets,
        )?;
        telemetry.stage_durations_ms.audio_encode = audio_encode_start
            .elapsed()
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
    }

    emit_progress(progress_tx, ExportStage::Mux, 95.0, 0.0, None);
    let mux_start = Instant::now();
    output.write_trailer().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to write output trailer: {err}"))
    })?;
    telemetry.stage_durations_ms.mux =
        mux_start.elapsed().as_millis().min(u128::from(u64::MAX)) as u64;
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
    scaler: &mut Option<ffmpeg::software::scaling::Context>,
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
            ffmpeg::software::scaling::Context::get(
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

fn send_video_frame_with_duration_and_progress(
    video_encoder: &mut ffmpeg::encoder::video::Encoder,
    output: &mut ffmpeg::format::context::Output,
    video_stream_index: usize,
    video_stream_time_base: ffmpeg::Rational,
    frame: &mut ffmpeg::frame::Video,
    frame_duration_ticks: usize,
    pending_packet_durations: &mut VecDeque<i64>,
    encoded_output_count: &Cell<usize>,
    total_frame_count: usize,
    start: &Instant,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<()> {
    let frame_duration_ticks = frame_duration_ticks.max(1);
    let output_index = encoded_output_count.get();
    frame.set_pts(Some(output_index as i64));
    video_encoder.send_frame(frame).map_err(|err| {
        ScreenRecorderError::Export(format!("failed to send frame to video encoder: {err}"))
    })?;
    pending_packet_durations.push_back(frame_duration_ticks as i64);
    drain_video_packets_with_durations(
        video_encoder,
        output,
        video_stream_index,
        video_stream_time_base,
        false,
        pending_packet_durations,
    )?;

    let next_count = output_index.saturating_add(frame_duration_ticks);
    encoded_output_count.set(next_count);

    if output_index.is_multiple_of(10) || next_count == total_frame_count {
        let elapsed = start.elapsed().as_secs_f32().max(0.001);
        let fps = next_count as f32 / elapsed;
        let remaining_frames = total_frame_count.saturating_sub(next_count) as f32;
        let eta_ms = if fps > 0.0 {
            Some(((remaining_frames / fps) * 1000.0) as u64)
        } else {
            None
        };
        let percent = 35.0 + ((next_count as f32 / total_frame_count.max(1) as f32) * 55.0);
        emit_progress(progress_tx, ExportStage::VideoEncode, percent, fps, eta_ms);
    }

    Ok(())
}

#[derive(Default)]
struct PendingCollapsedVideoFrame {
    frame: Option<ffmpeg::frame::Video>,
    duration_ticks: usize,
}

impl PendingCollapsedVideoFrame {
    fn store(&mut self, source: &ffmpeg::frame::Video, duration_ticks: usize) -> Result<()> {
        let needs_reallocate = self.frame.as_ref().is_none_or(|frame| {
            frame.format() != source.format()
                || frame.width() != source.width()
                || frame.height() != source.height()
        });
        if needs_reallocate {
            self.frame = Some(ffmpeg::frame::Video::new(
                source.format(),
                source.width(),
                source.height(),
            ));
        }

        let frame = self.frame.as_mut().ok_or_else(|| {
            ScreenRecorderError::Export(
                "pending collapsed video frame is uninitialized".to_string(),
            )
        })?;
        copy_video_frame_into_matching(frame, source)?;
        self.duration_ticks = duration_ticks.max(1);
        Ok(())
    }

    fn is_empty(&self) -> bool {
        self.duration_ticks == 0
    }

    fn clear(&mut self) {
        self.duration_ticks = 0;
    }
}

fn queue_video_frame_with_repeat_collapse(
    video_encoder: &mut ffmpeg::encoder::video::Encoder,
    output: &mut ffmpeg::format::context::Output,
    video_stream_index: usize,
    video_stream_time_base: ffmpeg::Rational,
    frame: &mut ffmpeg::frame::Video,
    frame_duration_ticks: usize,
    allow_repeat_collapse: bool,
    pending_collapsed_frame: &mut PendingCollapsedVideoFrame,
    pending_packet_durations: &mut VecDeque<i64>,
    encoded_output_count: &Cell<usize>,
    scheduled_output_count: &Cell<usize>,
    total_frame_count: usize,
    start: &Instant,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<()> {
    let frame_duration_ticks = frame_duration_ticks.max(1);
    scheduled_output_count.set(
        scheduled_output_count
            .get()
            .saturating_add(frame_duration_ticks),
    );

    if !allow_repeat_collapse || !can_collapse_repeated_video_frames(frame) {
        flush_pending_collapsed_video_frame(
            video_encoder,
            output,
            video_stream_index,
            video_stream_time_base,
            pending_collapsed_frame,
            pending_packet_durations,
            encoded_output_count,
            total_frame_count,
            start,
            progress_tx,
        )?;
        return send_video_frame_with_duration_and_progress(
            video_encoder,
            output,
            video_stream_index,
            video_stream_time_base,
            frame,
            frame_duration_ticks,
            pending_packet_durations,
            encoded_output_count,
            total_frame_count,
            start,
            progress_tx,
        );
    }

    if pending_collapsed_frame.is_empty() {
        return pending_collapsed_frame.store(frame, frame_duration_ticks);
    }

    let pending_frame = pending_collapsed_frame.frame.as_ref().ok_or_else(|| {
        ScreenRecorderError::Export("pending collapsed video frame is uninitialized".to_string())
    })?;
    if video_frames_match(pending_frame, frame) {
        pending_collapsed_frame.duration_ticks = pending_collapsed_frame
            .duration_ticks
            .saturating_add(frame_duration_ticks);
        return Ok(());
    }

    flush_pending_collapsed_video_frame(
        video_encoder,
        output,
        video_stream_index,
        video_stream_time_base,
        pending_collapsed_frame,
        pending_packet_durations,
        encoded_output_count,
        total_frame_count,
        start,
        progress_tx,
    )?;
    pending_collapsed_frame.store(frame, frame_duration_ticks)
}

fn flush_pending_collapsed_video_frame(
    video_encoder: &mut ffmpeg::encoder::video::Encoder,
    output: &mut ffmpeg::format::context::Output,
    video_stream_index: usize,
    video_stream_time_base: ffmpeg::Rational,
    pending_collapsed_frame: &mut PendingCollapsedVideoFrame,
    pending_packet_durations: &mut VecDeque<i64>,
    encoded_output_count: &Cell<usize>,
    total_frame_count: usize,
    start: &Instant,
    progress_tx: &Option<Sender<ExportProgress>>,
) -> Result<()> {
    if pending_collapsed_frame.is_empty() {
        return Ok(());
    }

    let duration_ticks = pending_collapsed_frame.duration_ticks;
    let frame = pending_collapsed_frame.frame.as_mut().ok_or_else(|| {
        ScreenRecorderError::Export("pending collapsed video frame is uninitialized".to_string())
    })?;
    send_video_frame_with_duration_and_progress(
        video_encoder,
        output,
        video_stream_index,
        video_stream_time_base,
        frame,
        duration_ticks,
        pending_packet_durations,
        encoded_output_count,
        total_frame_count,
        start,
        progress_tx,
    )?;
    pending_collapsed_frame.clear();
    Ok(())
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

fn copy_video_frame_into_matching(
    dst: &mut ffmpeg::frame::Video,
    src: &ffmpeg::frame::Video,
) -> Result<()> {
    if dst.format() != src.format() || dst.width() != src.width() || dst.height() != src.height() {
        return Err(ScreenRecorderError::Export(
            "video frame copy requires matching format and dimensions".to_string(),
        ));
    }

    ensure_video_frame_writable(dst)?;
    let status = unsafe { ffmpeg::ffi::av_frame_copy(dst.as_mut_ptr(), src.as_ptr()) };
    if status < 0 {
        return Err(ScreenRecorderError::Export(format!(
            "failed to copy video frame: {}",
            ffmpeg::Error::from(status)
        )));
    }
    Ok(())
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

fn select_video_codec(
    output: &ffmpeg::format::context::Output,
    output_path: &Path,
    format: ExportFormat,
    requested_codec: VideoCodec,
    mode: ExportExecutionMode,
    software_h264_priority: SoftwareH264Priority,
) -> Result<ffmpeg::Codec> {
    if matches!(format, ExportFormat::Webp) {
        return ffmpeg::encoder::find_by_name("libwebp_anim")
            .or_else(|| ffmpeg::encoder::find(ffmpeg::codec::Id::WEBP))
            .ok_or_else(|| {
                ScreenRecorderError::Export(
                    "no animated WebP encoder is available; FFmpeg must include libwebp"
                        .to_string(),
                )
            });
    }

    if matches!(format, ExportFormat::Mp4) {
        let (encoder_name, codec_name) = exact_mp4_encoder(requested_codec);
        if matches!(mode, ExportExecutionMode::HardwareOnly) {
            return Err(ScreenRecorderError::Export(format!(
                "HardwareOnly mode cannot satisfy the requested {codec_name} export; SnowShot requires {encoder_name}"
            )));
        }
        return ffmpeg::encoder::find_by_name(encoder_name).ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "requested {codec_name} encoder {encoder_name} is unavailable; rebuild FFmpeg with {encoder_name} support"
            ))
        });
    }

    let container_video_codec = output
        .format()
        .codec(output_path, ffmpeg::media::Type::Video);
    let preferred_video_codec = choose_video_codec_id(format, requested_codec);
    let codec = ffmpeg::encoder::find(preferred_video_codec)
        .or_else(|| ffmpeg::encoder::find(container_video_codec))
        .ok_or_else(|| {
            ScreenRecorderError::Export(format!(
                "no video encoder available for {format:?} (preferred={preferred_video_codec:?}, container={container_video_codec:?})"
            ))
        })?;

    if matches!(mode, ExportExecutionMode::SoftwareOnly) && is_hardware_video_encoder(&codec) {
        if let Some(software_codec) = select_software_h264_codec(software_h264_priority) {
            return Ok(software_codec);
        }
        return Err(ScreenRecorderError::Export(
            "SoftwareOnly mode requested, but no software H.264 encoder is available".to_string(),
        ));
    }

    if matches!(mode, ExportExecutionMode::HardwareOnly)
        && matches!(format, ExportFormat::Mp4)
        && !is_hardware_video_encoder(&codec)
    {
        return Err(ScreenRecorderError::Export(
            "HardwareOnly mode requested, but selected codec is not hardware accelerated"
                .to_string(),
        ));
    }

    Ok(codec)
}

fn exact_mp4_encoder(codec: VideoCodec) -> (&'static str, &'static str) {
    match codec {
        VideoCodec::H264 => ("libx264", "H.264"),
        VideoCodec::H265 => ("libx265", "H.265"),
    }
}

fn effective_video_config(video_config: &VideoEncodeConfig) -> VideoEncodeConfig {
    *video_config
}

fn configure_codec_threads(
    context: &mut ffmpeg::codec::context::Context,
    configured_threads: u8,
    kind: ffmpeg::codec::threading::Type,
) {
    let count = match configured_threads {
        0 => auto_thread_count_from_physical_cores(),
        value => usize::from(value),
    };

    let threading = ffmpeg::codec::threading::Config {
        kind,
        count: count.max(1),
    };
    context.set_threading(threading);
}

fn should_use_x264_options(codec: &ffmpeg::Codec) -> bool {
    codec.name().eq_ignore_ascii_case("libx264")
}

fn should_use_x265_options(codec: &ffmpeg::Codec) -> bool {
    codec.name().eq_ignore_ascii_case("libx265")
}

fn is_hardware_h264_encoder(codec: &ffmpeg::Codec) -> bool {
    let name = codec.name().to_ascii_lowercase();
    name.contains("nvenc") || name.contains("qsv") || name.contains("amf") || name.contains("mf")
}

fn is_hardware_video_encoder(codec: &ffmpeg::Codec) -> bool {
    is_hardware_h264_encoder(codec)
}

fn select_software_h264_codec(priority: SoftwareH264Priority) -> Option<ffmpeg::Codec> {
    let preferred = match priority {
        SoftwareH264Priority::OpenH264First => ["libopenh264", "libx264"],
        SoftwareH264Priority::X264First => ["libx264", "libopenh264"],
    };
    preferred
        .into_iter()
        .find_map(ffmpeg::encoder::find_by_name)
}

fn open_video_encoder(
    video_encoder: ffmpeg::codec::encoder::video::Video,
    codec: &ffmpeg::Codec,
    video_config: &VideoEncodeConfig,
) -> Result<ffmpeg::encoder::video::Encoder> {
    if should_use_x264_options(codec) {
        let mut options = ffmpeg::Dictionary::new();
        options.set("preset", x264_preset_for(video_config.speed));
        options.set("tune", "zerolatency");
        options.set("bf", "0");
        options.set(
            "crf",
            &quality_to_h264_crf(video_config.quality).to_string(),
        );
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open video encoder with h264 options: {err}"
            ))
        });
    }
    if should_use_x265_options(codec) {
        let mut options = ffmpeg::Dictionary::new();
        options.set("preset", video_config.speed.as_x264_preset());
        options.set(
            "crf",
            &quality_to_h264_crf(video_config.quality).to_string(),
        );
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open video encoder with h265 options: {err}"
            ))
        });
    }
    let mut options = ffmpeg::Dictionary::new();
    if apply_hardware_encoder_speed_options(&mut options, codec) {
        return video_encoder.open_as_with(*codec, options).map_err(|err| {
            ScreenRecorderError::Export(format!(
                "failed to open hardware video encoder with speed options: {err}"
            ))
        });
    }
    video_encoder
        .open_as(*codec)
        .map_err(|err| ScreenRecorderError::Export(format!("failed to open video encoder: {err}")))
}

fn apply_hardware_encoder_speed_options(
    options: &mut ffmpeg::Dictionary<'_>,
    codec: &ffmpeg::Codec,
) -> bool {
    let name = codec.name().to_ascii_lowercase();
    if name.contains("nvenc") {
        options.set("preset", "p1");
        options.set("tune", "ull");
        options.set("rc", "constqp");
        options.set("bf", "0");
        options.set("delay", "0");
        return true;
    }
    if name.contains("qsv") {
        options.set("preset", "veryfast");
        options.set("look_ahead", "0");
        options.set("async_depth", "1");
        options.set("bf", "0");
        return true;
    }
    if name.contains("amf") {
        options.set("usage", "transcoding");
        options.set("quality", "speed");
        options.set("bf", "0");
        return true;
    }
    if name.contains("mf") {
        options.set("bf", "0");
        options.set("g", "60");
        return true;
    }
    false
}

fn x264_preset_for(speed: VideoEncodingSpeed) -> &'static str {
    speed.as_x264_preset()
}

fn drain_video_packets_with_durations(
    encoder: &mut ffmpeg::encoder::video::Encoder,
    output: &mut ffmpeg::format::context::Output,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    draining: bool,
    pending_packet_durations: &mut VecDeque<i64>,
) -> Result<()> {
    loop {
        let mut packet = ffmpeg::Packet::empty();
        match encoder.receive_packet(&mut packet) {
            Ok(()) => {
                if let Some(duration) = pending_packet_durations.pop_front() {
                    packet.set_duration(duration);
                }
                packet.set_stream(stream_index);
                packet.rescale_ts(encoder.time_base(), stream_time_base);
                packet.write_interleaved(output).map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "failed to write encoded video packet: {err}"
                    ))
                })?;
            }
            Err(ffmpeg::Error::Eof) => break,
            Err(err) if is_eagain(&err) && !draining => break,
            Err(err) if is_eagain(&err) && draining => continue,
            Err(err) => {
                return Err(ScreenRecorderError::Export(format!(
                    "failed to receive encoded video packet: {err}"
                )));
            }
        }
    }
    Ok(())
}

fn drain_audio_packets_with_callback<F>(
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    draining: bool,
    mut on_packet: F,
) -> Result<()>
where
    F: FnMut(ffmpeg::Packet) -> Result<()>,
{
    loop {
        let mut packet = ffmpeg::Packet::empty();
        match encoder.receive_packet(&mut packet) {
            Ok(()) => on_packet(packet)?,
            Err(ffmpeg::Error::Eof) => break,
            Err(err) if is_eagain(&err) && !draining => break,
            Err(err) if is_eagain(&err) && draining => continue,
            Err(err) => {
                return Err(ScreenRecorderError::Export(format!(
                    "failed to receive encoded audio packet: {err}"
                )));
            }
        }
    }
    Ok(())
}

struct BufferedAudioPackets {
    encoder_time_base: ffmpeg::Rational,
    packets: Vec<ffmpeg::Packet>,
}

struct AudioPacketWorker {
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    handle: thread::JoinHandle<Result<BufferedAudioPackets>>,
}

struct AudioPacketWorkerGuard {
    worker: Option<AudioPacketWorker>,
    cancel_flag: Arc<AtomicBool>,
}

impl AudioPacketWorkerGuard {
    fn new(worker: Option<AudioPacketWorker>, cancel_flag: Arc<AtomicBool>) -> Self {
        Self {
            worker,
            cancel_flag,
        }
    }

    fn take(&mut self) -> Option<AudioPacketWorker> {
        self.worker.take()
    }
}

impl Drop for AudioPacketWorkerGuard {
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            self.cancel_flag.store(true, Ordering::Release);
            let _ = join_audio_packet_worker(worker);
        }
    }
}

fn spawn_audio_packet_worker(
    audio_encoder: ffmpeg::encoder::audio::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    mixed: AudioMixPlan,
    cancel_flag: Arc<AtomicBool>,
) -> Result<AudioPacketWorker> {
    let handle = thread::Builder::new()
        .name("snow-export-audio-encode".to_string())
        .spawn(move || {
            let mut audio_encoder = audio_encoder;
            let encoder_time_base = audio_encoder.time_base();
            let packets =
                collect_encoded_audio_packets(&mut audio_encoder, &mixed, Some(&cancel_flag))?;
            Ok(BufferedAudioPackets {
                encoder_time_base,
                packets,
            })
        })
        .map_err(|err| ScreenRecorderError::Io(std::io::Error::other(err)))?;
    Ok(AudioPacketWorker {
        stream_index,
        stream_time_base,
        handle,
    })
}

fn join_audio_packet_worker(
    worker: AudioPacketWorker,
) -> Result<(BufferedAudioPackets, usize, ffmpeg::Rational)> {
    let stream_index = worker.stream_index;
    let stream_time_base = worker.stream_time_base;
    let buffered = worker.handle.join().map_err(|panic| {
        let message = if let Some(msg) = panic.downcast_ref::<&str>() {
            (*msg).to_string()
        } else if let Some(msg) = panic.downcast_ref::<String>() {
            msg.clone()
        } else {
            "audio encode worker panicked".to_string()
        };
        ScreenRecorderError::Export(message)
    })??;
    Ok((buffered, stream_index, stream_time_base))
}

fn collect_encoded_audio_packets(
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    mixed: &AudioMixPlan,
    cancel_flag: Option<&Arc<AtomicBool>>,
) -> Result<Vec<ffmpeg::Packet>> {
    let input_layout = ffmpeg::ChannelLayout::default(i32::from(mixed.channels.max(1)));
    let encoder_format = encoder.format();
    let direct_render_target = if encoder.rate() == mixed.sample_rate_hz.max(1)
        && encoder.channel_layout() == input_layout
    {
        match encoder_format {
            ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed) => {
                Some(AudioDirectRenderTarget::I16Packed)
            }
            _ => None,
        }
    } else {
        None
    };
    let mut resampler = if direct_render_target.is_none() {
        Some(
            ffmpeg::software::resampling::Context::get(
                ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
                input_layout,
                mixed.sample_rate_hz.max(1),
                encoder_format,
                encoder.channel_layout(),
                encoder.rate(),
            )
            .map_err(|err| {
                ScreenRecorderError::Export(format!("failed to create encode resampler: {err}"))
            })?,
        )
    } else {
        None
    };

    let frame_samples = {
        let fs = encoder.frame_size() as usize;
        if fs == 0 { 1024 } else { fs }
    };
    let variable_frame_size = encoder.frame_size() == 0;
    let input_channels = usize::from(mixed.channels.max(1));
    let frames_per_chunk = if variable_frame_size {
        (mixed.sample_rate_hz as usize / 25).max(1)
    } else {
        frame_samples
    };

    let mut next_pts = 0i64;
    let mut frame_cursor = 0usize;
    let mut renderer = AudioMixRenderer::new(mixed)?;
    let mut source = ffmpeg::frame::Audio::new(
        direct_render_target
            .map(AudioDirectRenderTarget::sample_format)
            .unwrap_or(ffmpeg::format::Sample::I16(
                ffmpeg::format::sample::Type::Packed,
            )),
        frames_per_chunk,
        if direct_render_target.is_some() {
            encoder.channel_layout()
        } else {
            input_layout
        },
    );
    source.set_rate(if direct_render_target.is_some() {
        encoder.rate()
    } else {
        mixed.sample_rate_hz.max(1)
    });
    let mut converted = ffmpeg::frame::Audio::empty();
    #[cfg(target_endian = "big")]
    let mut rendered_chunk = vec![0i16; frames_per_chunk * input_channels];
    let estimated_packet_count = mixed
        .target_frames
        .div_ceil(frame_samples.max(1))
        .saturating_add(8);
    let mut packets = Vec::with_capacity(estimated_packet_count);

    while frame_cursor < mixed.target_frames {
        if let Some(flag) = cancel_flag {
            check_canceled(flag)?;
        }
        let remaining_frames = mixed.target_frames - frame_cursor;
        let take_frames = remaining_frames.min(frames_per_chunk);
        let render_frames = if variable_frame_size {
            take_frames
        } else {
            frames_per_chunk
        };
        if render_frames == 0 {
            break;
        }
        frame_cursor += take_frames;

        source.set_samples(render_frames);
        let render_sample_count = render_frames * input_channels;
        let direct_frame_ready = if let Some(target) = direct_render_target {
            render_audio_chunk_direct(
                &mut renderer,
                frame_cursor - take_frames,
                render_frames,
                input_channels,
                target,
                &mut source,
                #[cfg(target_endian = "big")]
                &mut rendered_chunk,
            )?;
            true
        } else {
            let expected_bytes = render_sample_count * 2;
            let source_data = source.data_mut(0);
            if source_data.len() < expected_bytes {
                return Err(ScreenRecorderError::Export(
                    "allocated source audio frame is too small".to_string(),
                ));
            }

            #[cfg(target_endian = "little")]
            {
                let source_samples = unsafe {
                    std::slice::from_raw_parts_mut(
                        source_data.as_mut_ptr() as *mut i16,
                        source_data.len() / std::mem::size_of::<i16>(),
                    )
                };
                renderer.render_into(
                    frame_cursor - take_frames,
                    render_frames,
                    &mut source_samples[..render_sample_count],
                )?;
            }
            #[cfg(target_endian = "big")]
            {
                rendered_chunk.resize(render_sample_count, 0);
                renderer.render_into(
                    frame_cursor - take_frames,
                    render_frames,
                    &mut rendered_chunk,
                )?;
                for (i, sample) in rendered_chunk.iter().enumerate() {
                    let bytes = sample.to_le_bytes();
                    source_data[i * 2] = bytes[0];
                    source_data[i * 2 + 1] = bytes[1];
                }
            }

            resampler
                .as_mut()
                .ok_or_else(|| {
                    ScreenRecorderError::Export(
                        "audio encode resampler is unexpectedly uninitialized".to_string(),
                    )
                })?
                .run(&source, &mut converted)
                .map_err(|err| {
                    ScreenRecorderError::Export(format!(
                        "failed to resample audio for encoding: {err}"
                    ))
                })?;

            if converted.samples() == 0 {
                continue;
            }
            false
        };

        let sample_count = if direct_frame_ready {
            source.samples()
        } else {
            converted.samples()
        };
        let current_pts = next_pts;
        next_pts = next_pts.saturating_add(sample_count as i64);
        if direct_frame_ready {
            source.set_pts(Some(current_pts));
            encoder.send_frame(&source)
        } else {
            converted.set_pts(Some(current_pts));
            encoder.send_frame(&converted)
        }
        .map_err(|err| {
            ScreenRecorderError::Export(format!("failed to send audio frame to encoder: {err}"))
        })?;
        drain_audio_packets_with_callback(encoder, false, |packet| {
            packets.push(packet);
            Ok(())
        })?;
    }

    if let Some(flag) = cancel_flag {
        check_canceled(flag)?;
    }
    encoder.send_eof().map_err(|err| {
        ScreenRecorderError::Export(format!("failed to finalize audio encoder: {err}"))
    })?;
    drain_audio_packets_with_callback(encoder, true, |packet| {
        packets.push(packet);
        Ok(())
    })?;
    Ok(packets)
}

#[derive(Clone, Copy, Debug)]
enum AudioDirectRenderTarget {
    I16Packed,
}

impl AudioDirectRenderTarget {
    fn sample_format(self) -> ffmpeg::format::Sample {
        match self {
            Self::I16Packed => ffmpeg::format::Sample::I16(ffmpeg::format::sample::Type::Packed),
        }
    }
}

#[allow(unused_variables)]
fn render_audio_chunk_direct(
    renderer: &mut AudioMixRenderer,
    start_output_frame: usize,
    render_frames: usize,
    input_channels: usize,
    target: AudioDirectRenderTarget,
    source: &mut ffmpeg::frame::Audio,
    #[cfg(target_endian = "big")] rendered_chunk_i16: &mut Vec<i16>,
) -> Result<()> {
    match target {
        AudioDirectRenderTarget::I16Packed => {
            let render_sample_count = render_frames * input_channels;
            let expected_bytes = render_sample_count * 2;
            let source_data = source.data_mut(0);
            if source_data.len() < expected_bytes {
                return Err(ScreenRecorderError::Export(
                    "allocated source audio frame is too small".to_string(),
                ));
            }

            #[cfg(target_endian = "little")]
            {
                let source_samples = unsafe {
                    std::slice::from_raw_parts_mut(
                        source_data.as_mut_ptr() as *mut i16,
                        source_data.len() / std::mem::size_of::<i16>(),
                    )
                };
                renderer.render_into(
                    start_output_frame,
                    render_frames,
                    &mut source_samples[..render_sample_count],
                )?;
            }
            #[cfg(target_endian = "big")]
            {
                rendered_chunk_i16.resize(render_sample_count, 0);
                renderer.render_into(
                    start_output_frame,
                    render_frames,
                    &mut rendered_chunk_i16[..render_sample_count],
                )?;
                for (i, sample) in rendered_chunk_i16.iter().enumerate() {
                    let bytes = sample.to_le_bytes();
                    source_data[i * 2] = bytes[0];
                    source_data[i * 2 + 1] = bytes[1];
                }
            }
        }
    }

    Ok(())
}

fn write_buffered_audio_packets(
    output: &mut ffmpeg::format::context::Output,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    encoder_time_base: ffmpeg::Rational,
    packets: &mut [ffmpeg::Packet],
) -> Result<()> {
    for packet in packets {
        packet.set_stream(stream_index);
        packet.rescale_ts(encoder_time_base, stream_time_base);
        packet.write_interleaved(output).map_err(|err| {
            ScreenRecorderError::Export(format!("failed to write encoded audio packet: {err}"))
        })?;
    }
    Ok(())
}

fn encode_audio_samples(
    output: &mut ffmpeg::format::context::Output,
    encoder: &mut ffmpeg::encoder::audio::Encoder,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    mixed: &AudioMixPlan,
) -> Result<()> {
    let encoder_time_base = encoder.time_base();
    let mut packets = collect_encoded_audio_packets(encoder, mixed, None)?;
    write_buffered_audio_packets(
        output,
        stream_index,
        stream_time_base,
        encoder_time_base,
        &mut packets,
    )
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
            asset_id: format!("audio/{track_id}.wav"),
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
                asset_id: Some("audio/system.wav".to_string()),
                offset: 0,
                len: 0,
            });
        }
        if microphone {
            assets.push(BundleAssetRecord {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some("audio/microphone.wav".to_string()),
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
            collect_required_source_indices(&[0, 0, 1, 1, 1, 3, 3, 8]),
            vec![0, 1, 3, 8]
        );
        assert!(collect_required_source_indices(&[]).is_empty());
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
            ["apng", "avi", "gif", "matroska", "mov", "mp4", "webp"],
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
                2,
                10,
                format,
                codec,
                None,
                8,
                &VideoEncodeConfig::default(),
                &performance,
                &cancel_flag,
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
    fn packet_copy_path_requires_non_overlay_and_matching_fps() {
        assert!(can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            1.0,
            60,
            60,
            false,
            false,
            true,
        ));
        assert!(can_use_video_packet_copy_path(
            ExportFormat::Avi,
            0.5,
            24,
            24,
            false,
            false,
            true,
        ));

        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            1.0,
            60,
            60,
            true,
            false,
            true,
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Gif,
            1.0,
            20,
            20,
            false,
            false,
            true,
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            1.0,
            60,
            30,
            false,
            false,
            true,
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            0.0,
            60,
            60,
            false,
            false,
            true,
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            f32::NAN,
            60,
            60,
            false,
            false,
            true,
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            1.0,
            60,
            60,
            false,
            true,
            true
        ));
        assert!(!can_use_video_packet_copy_path(
            ExportFormat::Mp4,
            1.0,
            60,
            60,
            false,
            false,
            false
        ));
    }

    #[test]
    fn direct_passthrough_requires_mp4_no_audio_unity_speed_and_no_overlay() {
        let editing = test_editing_session(false, false);
        let mut request = editing.export_request();
        request.format = ExportFormat::Mp4;
        request.output_path = PathBuf::from("recordings/output.mp4");
        request.playback_speed = 1.0;

        assert!(can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            false
        ));

        request.audio_tracks.push(ExportAudioTrackRequest {
            track_id: "system".to_string(),
            enabled: true,
            volume: 1.0,
        });
        assert!(!can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            false
        ));
        request.audio_tracks.clear();

        request.playback_speed = 1.25;
        assert!(!can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            false
        ));
        request.playback_speed = 1.0;

        request.format = ExportFormat::Avi;
        assert!(!can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            false
        ));
        request.format = ExportFormat::Mp4;

        request.output_path = PathBuf::from("recordings/output.avi");
        assert!(!can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            false
        ));
        request.output_path = PathBuf::from("recordings/output.mp4");

        assert!(!can_use_direct_video_passthrough_copy_path(
            &editing.manifest,
            &request,
            true
        ));
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
    fn apply_mouse_overlays_uses_fallback_for_fully_transparent_alpha_shape() {
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

        let cursor_pixel = (8usize * 16 + 8usize) * 4;
        assert!(
            frame.rgba[cursor_pixel + 3] > 0,
            "fallback cursor should draw even when sampled shape is fully transparent"
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
    fn apply_mouse_overlays_uses_fallback_for_noop_masked_shape() {
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

        let cursor_pixel = (8usize * 16 + 8usize) * 4;
        assert!(
            frame.rgba[cursor_pixel + 3] > 0,
            "fallback cursor should draw when masked shape pixels are all no-op"
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
    fn native_overlay_path_draws_cursor_on_yuv420p_frame() {
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
            try_apply_mouse_overlays_native_from_decision(
                &mut frame, 0, &tracks, &config, &mut state, decision,
            )
            .unwrap()
        );
        assert!(
            frame.data(0).iter().any(|&value| value != 16),
            "native YUV420P overlay should modify luma plane"
        );
    }

    #[test]
    fn native_overlay_path_draws_cursor_on_nv12_frame() {
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
            try_apply_mouse_overlays_native_from_decision(
                &mut frame, 0, &tracks, &config, &mut state, decision,
            )
            .unwrap()
        );
        assert!(
            frame.data(0).iter().any(|&value| value != 16),
            "native NV12 overlay should modify luma plane"
        );
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
}

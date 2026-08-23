use std::error::Error;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use snow_screen_recorder::{
    EditingSession, ExportExecutionMode, ExportFormat, LocalRecordingPaths, RecordingArtifact,
    SoftwareH264Priority,
};

fn main() -> Result<(), Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let bundle_path = args
        .next()
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("./recordings/session.snowrec"));
    let mut enable_overlay = false;
    let mut mouse_visible = false;
    let mut mouse_trail = false;
    let mut mouse_click = false;
    let mut enable_system_audio = false;
    let mut enable_microphone_audio = false;
    let mut playback_speed = 1.0f32;
    let mut format = ExportFormat::Mp4;
    let mut execution_mode = ExportExecutionMode::HardwarePreferred;
    let mut software_h264_priority = SoftwareH264Priority::X264First;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--overlay" => {
                enable_overlay = true;
                mouse_visible = true;
                mouse_trail = true;
                mouse_click = true;
            }
            "--cursor-only" => {
                enable_overlay = true;
                mouse_visible = true;
            }
            "--trail-only" => {
                enable_overlay = true;
                mouse_trail = true;
            }
            "--click-only" => {
                enable_overlay = true;
                mouse_click = true;
            }
            "--system-audio" => enable_system_audio = true,
            "--microphone-audio" => enable_microphone_audio = true,
            "--speed" => {
                let value = args.next().ok_or("--speed requires a value")?;
                playback_speed = value.parse()?;
            }
            "--format" => {
                let value = args.next().ok_or("--format requires a value")?;
                format = parse_format(&value)?;
            }
            "--software-only" => execution_mode = ExportExecutionMode::SoftwareOnly,
            "--hardware-only" => execution_mode = ExportExecutionMode::HardwareOnly,
            "--hardware-preferred" => execution_mode = ExportExecutionMode::HardwarePreferred,
            "--openh264-first" => software_h264_priority = SoftwareH264Priority::OpenH264First,
            "--x264-first" => software_h264_priority = SoftwareH264Priority::X264First,
            other => return Err(format!("unknown arg: {other}").into()),
        }
    }

    let artifact = RecordingArtifact {
        session_id: bundle_path
            .file_stem()
            .and_then(|stem| stem.to_str())
            .unwrap_or("session")
            .to_string(),
        output_dir: bundle_path
            .parent()
            .map(|path| path.to_path_buf())
            .unwrap_or_else(|| PathBuf::from(".")),
        local_paths: LocalRecordingPaths {
            temp_dir: std::env::temp_dir().join("snow-export-benchmark"),
            video_intermediate_path: bundle_path.clone(),
            video_index_path: PathBuf::new(),
            mouse_path: PathBuf::new(),
        },
        bundle_path: bundle_path.clone(),
        audio_tracks: Vec::new(),
    };
    let manifest = artifact.read_embedded_manifest()?;
    let output_path = manifest.output_dir.join(format!(
        "{}-benchmark.{}",
        manifest.session_id,
        format.file_extension()
    ));

    let editing = EditingSession::open(artifact)?;
    let mut request = editing.export_request();
    request.format = format;
    request.output_path = output_path;
    request.performance.mode = execution_mode;
    request.performance.software_h264_priority = software_h264_priority;
    request.playback_speed = playback_speed;
    for track in &mut request.audio_tracks {
        if track.track_id == "system" {
            track.enabled = enable_system_audio;
        } else if track.track_id == "microphone" {
            track.enabled = enable_microphone_audio;
        }
    }
    if enable_overlay {
        request.mouse.visible = mouse_visible;
        request.mouse.click_enabled = mouse_click;
        request.mouse.trail_enabled = mouse_trail;
    }

    let started_at = Instant::now();
    let task = editing.export_async(request)?;
    let progress_rx = task.progress();

    while let Ok(progress) = progress_rx.recv_timeout(Duration::from_millis(250)) {
        println!(
            "stage={:?} percent={:.1} fps={:.1} queue={:.2} mem={}MB",
            progress.stage,
            progress.percent,
            progress.video_fps,
            progress.queue_utilization,
            progress.peak_memory_mb
        );
    }

    let result = task.wait()?;
    println!(
        "Export finished: {} in {:.2}s",
        result.output_path.display(),
        started_at.elapsed().as_secs_f64()
    );
    println!(
        "path={:?} hw_decode={} hw_compose={} hw_encode={} stages_ms={{plan:{}, decode:{}, compose:{}, video:{}, audio:{}, mux:{}, finalize:{}}}",
        result.runtime_report.path,
        result.runtime_report.used_hardware_decode,
        result.runtime_report.used_hardware_compose,
        result.runtime_report.used_hardware_encode,
        result.runtime_report.stage_durations_ms.plan,
        result.runtime_report.stage_durations_ms.decode,
        result.runtime_report.stage_durations_ms.compose,
        result.runtime_report.stage_durations_ms.video_encode,
        result.runtime_report.stage_durations_ms.audio_encode,
        result.runtime_report.stage_durations_ms.mux,
        result.runtime_report.stage_durations_ms.finalize,
    );
    Ok(())
}

fn parse_format(value: &str) -> Result<ExportFormat, Box<dyn Error>> {
    match value.to_ascii_lowercase().as_str() {
        "mp4" => Ok(ExportFormat::Mp4),
        "avi" => Ok(ExportFormat::Avi),
        "gif" => Ok(ExportFormat::Gif),
        "apng" => Ok(ExportFormat::Apng),
        "webp" => Ok(ExportFormat::Webp),
        _ => Err(format!("unsupported format: {value}").into()),
    }
}

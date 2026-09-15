//! Export a version-2 editable bundle. Export does not require capture permission.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    use snow_recording_export::{EditingSession, ExportExecutionMode, ExportFormat, VideoCodec};
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.len() < 2 {
        return Err("usage: macos_export INPUT.snowrec OUTPUT [hevc|h264|gif|apng|webp]".into());
    }
    let editing = EditingSession::open(snow_recording_model::RecordingArtifact::open(
        args[0].clone().into(),
    )?)?;
    let mut request = editing.export_request();
    request.output_path = args[1].clone().into();
    request.performance.mode = ExportExecutionMode::HardwarePreferred;
    request.prefer_hardware_h264 = true;
    for track in &mut request.audio_tracks {
        track.enabled = true;
    }
    match args.get(2).map(String::as_str) {
        Some("hevc") => request.codec = VideoCodec::H265,
        Some("h264") => request.codec = VideoCodec::H264,
        Some("gif") => request.format = ExportFormat::Gif,
        Some("apng") => request.format = ExportFormat::Apng,
        Some("webp") => request.format = ExportFormat::Webp,
        None => {}
        _ => return Err("unknown export format".into()),
    }
    let result = editing.export(request)?;
    println!(
        "output={} duration_ms={} path={:?}",
        result.output_path.display(),
        result.duration_ms,
        result.runtime_report.path
    );
    Ok(())
}

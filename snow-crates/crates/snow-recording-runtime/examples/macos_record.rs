#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("This example requires macOS 15 or later.");
}
#[cfg(target_os = "macos")]
fn main() {
    use snow_macos::desktop::{DesktopConfig, DesktopTarget};
    use snow_recording_export::{ExportExecutionMode, VideoCodec};
    use snow_recording_runtime::macos::{NativeRecordingConfig, NativeRecordingSession};
    use std::time::{Duration, Instant};
    let args: Vec<_> = std::env::args().skip(1).collect();
    let path = args
        .first()
        .expect("usage: macos_record OUTPUT.mp4 [hdr] [system-audio] [separate] [effects] [pause] [editable] [microphone] [software|preferred]")
        .clone();
    let worker = std::thread::spawn(
        move || -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
            let mut capture = DesktopConfig::new(DesktopTarget::PrimaryDisplay);
            let hdr = args.iter().any(|arg| arg == "hdr");
            if hdr {
                capture.dynamic_range = snow_media::DynamicRange::Hdr;
            }
            if args.iter().any(|arg| arg == "separate") {
                capture.cursor = snow_media::CursorMode::Separate;
            }
            let audio = args
                .iter()
                .any(|arg| arg == "system-audio" || arg == "microphone")
                .then(|| {
                    let mut config = snow_audio_recorder::AudioStreamConfig::default();
                    config.system.enabled = args.iter().any(|arg| arg == "system-audio");
                    config.microphone.enabled = args.iter().any(|arg| arg == "microphone");
                    config
                });
            let config = NativeRecordingConfig {
                effects: snow_recording_runtime::macos::NativeEffectsConfig {
                    clicks: args.iter().any(|arg| arg == "effects"),
                    trail: args.iter().any(|arg| arg == "effects"),
                    keyboard: None,
                    ..Default::default()
                },
                capture,
                output: snow_media::geometry::PixelSize::new(1280, 720)?,
                output_path: path.into(),
                fps: 30,
                codec: if hdr {
                    VideoCodec::H265
                } else {
                    VideoCodec::H264
                },
                execution: if args.iter().any(|arg| arg == "software") {
                    ExportExecutionMode::SoftwareOnly
                } else if args.iter().any(|arg| arg == "preferred") {
                    ExportExecutionMode::HardwarePreferred
                } else {
                    ExportExecutionMode::HardwareOnly
                },
                audio,
            };
            let mut recording = if args.iter().any(|arg| arg == "editable") {
                Session::Editable(Box::new(
                    snow_recording_runtime::macos::NativeEditableSession::start(config)?,
                ))
            } else {
                Session::Direct(Box::new(NativeRecordingSession::start(config)?))
            };
            let begin = Instant::now();
            let mut paused = false;
            let mut resumed = false;
            while begin.elapsed() < Duration::from_secs(3) {
                if args.iter().any(|arg| arg == "pause") {
                    if !paused && begin.elapsed() >= Duration::from_secs(1) {
                        recording.pause();
                        paused = true;
                    }
                    if !resumed && begin.elapsed() >= Duration::from_millis(1500) {
                        recording.resume();
                        resumed = true;
                    }
                }
                recording.step(Duration::from_millis(20))?;
                if paused && !resumed {
                    std::thread::sleep(Duration::from_millis(5));
                }
            }
            let report = recording.finish()?;
            println!(
                "encoder={} encoded_frames={} cpu_readbacks={} configuration_changes={} interruptions={}",
                report.encoder.video_encoder,
                report.encoder.encoded_frames,
                report.cpu_readbacks,
                report.geometry_changes.len(),
                report.interruptions.len()
            );
            Ok(())
        },
    );
    snow_macos::run_loop::drive_until(|| worker.is_finished());
    if let Err(error) = worker.join().expect("recording worker panicked") {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(target_os = "macos")]
enum Session {
    Direct(Box<snow_recording_runtime::macos::NativeRecordingSession>),
    Editable(Box<snow_recording_runtime::macos::NativeEditableSession>),
}
#[cfg(target_os = "macos")]
impl Session {
    fn step(
        &mut self,
        timeout: std::time::Duration,
    ) -> Result<
        snow_recording_runtime::macos::NativeRecordingEvent,
        snow_recording_runtime::ScreenRecorderError,
    > {
        match self {
            Self::Direct(s) => s.step(timeout),
            Self::Editable(s) => s.step(timeout),
        }
    }
    fn pause(&mut self) {
        match self {
            Self::Direct(s) => s.pause(),
            Self::Editable(s) => s.pause(),
        }
    }
    fn resume(&mut self) {
        match self {
            Self::Direct(s) => s.resume(),
            Self::Editable(s) => s.resume(),
        }
    }
    fn finish(
        self,
    ) -> Result<
        snow_recording_runtime::macos::NativeRecordingReport,
        snow_recording_runtime::ScreenRecorderError,
    > {
        match self {
            Self::Direct(s) => s.finish(),
            Self::Editable(s) => s.finish().map(|r| r.recording),
        }
    }
}

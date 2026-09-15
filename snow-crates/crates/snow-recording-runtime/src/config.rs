use std::path::PathBuf;

pub use snow_capture::backend::CaptureBackendKind;
pub use snow_recording_model::{
    AudioTrackRole, IntermediateRecordingProfile, VideoEncodeConfig, VideoEncodingSpeed,
};

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct MonitorSelector {
    pub stable_id: String,
}

impl MonitorSelector {
    pub fn new(stable_id: impl Into<String>) -> Self {
        Self {
            stable_id: stable_id.into(),
        }
    }
}

pub use snow_capture::WindowId;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecordingRegion {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl RecordingRegion {
    pub fn new(x: i32, y: i32, width: u32, height: u32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }
}

#[derive(Clone, Debug)]
pub enum RecordingTarget {
    PrimaryMonitor,
    Monitor(MonitorSelector),
    Window(WindowId),
    Region(RecordingRegion),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AudioChannels {
    Mono,
    #[default]
    Stereo,
}

impl AudioChannels {
    pub const fn channels(self) -> u16 {
        match self {
            Self::Mono => 1,
            Self::Stereo => 2,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RecordingAudioTrackSource {
    SystemDefault,
    MicrophoneDefault,
    InputDeviceId(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RecordingAudioTrackConfig {
    pub track_id: String,
    pub role: AudioTrackRole,
    pub source: RecordingAudioTrackSource,
    pub enabled: bool,
}

impl RecordingAudioTrackConfig {
    pub fn system_default(track_id: impl Into<String>) -> Self {
        Self {
            track_id: track_id.into(),
            role: AudioTrackRole::SystemOutput,
            source: RecordingAudioTrackSource::SystemDefault,
            enabled: true,
        }
    }

    pub fn microphone_default(track_id: impl Into<String>) -> Self {
        Self {
            track_id: track_id.into(),
            role: AudioTrackRole::MicrophoneInput,
            source: RecordingAudioTrackSource::MicrophoneDefault,
            enabled: true,
        }
    }
}

#[derive(Clone, Debug)]
pub struct RecordingAudioConfig {
    pub channels: AudioChannels,
    pub sample_rate_hz: u32,
    pub tracks: Vec<RecordingAudioTrackConfig>,
}

impl Default for RecordingAudioConfig {
    fn default() -> Self {
        Self {
            channels: AudioChannels::Stereo,
            sample_rate_hz: 48_000,
            tracks: vec![
                RecordingAudioTrackConfig::system_default("system"),
                RecordingAudioTrackConfig::microphone_default("microphone"),
            ],
        }
    }
}

#[derive(Clone, Debug)]
pub struct RecordingConfig {
    pub target: RecordingTarget,
    pub capture_backend: CaptureBackendKind,
    pub output_dir: PathBuf,
    pub keep_temp_files: bool,
    pub fps: u32,
    pub intermediate_profile: IntermediateRecordingProfile,
    pub video: VideoEncodeConfig,
    pub audio: RecordingAudioConfig,
}

impl RecordingConfig {
    pub fn validate(&self) -> Result<(), String> {
        if self.fps == 0 {
            return Err("fps must be > 0".to_string());
        }

        if self.output_dir.as_os_str().is_empty() {
            return Err("output_dir must not be empty".to_string());
        }

        self.video.validate("video")?;

        if self.audio.sample_rate_hz == 0 {
            return Err("audio sample rate must be > 0".to_string());
        }

        if self.audio.tracks.is_empty() {
            return Err("audio.tracks must not be empty".to_string());
        }

        Ok(())
    }
}

impl Default for RecordingConfig {
    fn default() -> Self {
        Self {
            target: RecordingTarget::PrimaryMonitor,
            capture_backend: CaptureBackendKind::Auto,
            output_dir: PathBuf::from("."),
            keep_temp_files: false,
            fps: 60,
            intermediate_profile: IntermediateRecordingProfile::EditFast,
            video: VideoEncodeConfig::default(),
            audio: RecordingAudioConfig::default(),
        }
    }
}

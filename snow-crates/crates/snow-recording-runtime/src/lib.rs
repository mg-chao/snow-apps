pub mod config;
pub mod direct;
pub mod error;
pub mod recording;

pub(crate) mod adapter;
pub(crate) mod ffmpeg_util;
pub(crate) mod laser_trail;
pub(crate) mod mouse_hook;
pub(crate) mod processor;
pub(crate) mod temp;
pub(crate) mod video_quality;

pub use config::{
    AudioChannels, CaptureBackendKind, MonitorSelector, RecordingAudioConfig,
    RecordingAudioTrackConfig, RecordingAudioTrackSource, RecordingConfig, RecordingRegion,
    RecordingTarget, WindowSelector,
};
pub use direct::{DirectRecordingConfig, DirectRecordingReport, DirectRecordingSession};
pub use error::ScreenRecorderError;
pub use recording::{RecordingSession, RecordingState};

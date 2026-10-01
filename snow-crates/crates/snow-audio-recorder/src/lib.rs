pub mod backend;
#[cfg(feature = "bench-internals")]
pub mod benchmark;
mod control;
pub mod device;
pub mod error;
pub mod format;
mod monitor;
pub mod packet;
mod platform;
pub mod recording;
pub mod session;
pub mod streaming;
pub mod timeline;

pub use backend::AudioBackendKind;
pub use control::{
    AudioControlHandle, AudioLevelSnapshot, AudioLevelsSnapshot, AudioSourceStatus, MAX_GAIN_DB,
    METER_STALE_MS, MIN_GAIN_DB,
};
pub use device::{AudioDeviceInfo, DeviceFlow, DeviceSelector};
pub use error::{
    AudioError, AudioErrorClass, AudioResult, RecvError, RecvTimeoutError, TryRecvError,
};
pub use format::{AudioFormat, MAX_CHANNELS};
pub use monitor::AudioMonitor;
pub use packet::{AudioEvent, AudioPacket, AudioPacketMetadata, AudioSourceKind};
pub use recording::{
    AudioRecordingArtifact, AudioRecordingConfig, AudioRecordingSession, AudioTrackConfig,
    AudioTrackDevice, RecordedAudioTrack,
};
pub use session::{
    AudioSession, AudioSessionBuilder, AudioStreamConfig, RestartPolicy, SourceConfig,
};
pub use streaming::{AudioStreamHandle, AudioStreamStats, AudioStreamStatsSnapshot};
pub use timeline::{
    AudioPacketAlignment, AudioPacketTimestamp, AudioTimestampAnchorExt,
    align_i16_interleaved_to_duration, align_packet_frames, audio_anchor_from_first_packet,
    audio_anchor_from_origin, audio_anchor_from_origin_instant, duration_to_frames_round,
};

#[cfg(any(windows, target_os = "macos"))]
mod convert;

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::error::Result;
use crate::read_recording_bundle_footer;
use crate::shared::{IntermediateRecordingProfile, VideoEncodeConfig};

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum AudioTrackRole {
    SystemOutput,
    MicrophoneInput,
    Auxiliary,
}

/// Sample encoding of a recorded audio track.
///
/// Audio track assets are stored as raw interleaved samples in this encoding;
/// there is no container header, so this enum (together with
/// `AudioTrackManifest::channels`) is what a reader needs to interpret the
/// bytes.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum AudioSampleFormat {
    /// Signed 16-bit little-endian PCM.
    PcmS16Le,
}

impl AudioSampleFormat {
    /// Size in bytes of one sample of a single channel.
    pub const fn bytes_per_sample(self) -> u16 {
        match self {
            Self::PcmS16Le => 2,
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct PauseInterval {
    pub start_ms: u64,
    pub end_ms: u64,
}

/// Describes one audio track asset inside a recording bundle.
///
/// This manifest is the single source of format metadata for the asset: the
/// asset bytes are `duration_frames` frames of `channels` interleaved samples
/// in `sample_format`, with nothing before or after them.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct AudioTrackManifest {
    pub track_id: String,
    pub role: AudioTrackRole,
    pub asset_id: String,
    pub sample_rate_hz: u32,
    pub channels: u16,
    pub sample_format: AudioSampleFormat,
    pub duration_frames: u64,
    pub recorded: bool,
}

impl AudioTrackManifest {
    /// Size in bytes of one interleaved frame (all channels of one sample
    /// instant) of this track.
    pub fn frame_bytes(&self) -> u64 {
        u64::from(self.channels.max(1)) * u64::from(self.sample_format.bytes_per_sample())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SessionManifest {
    pub session_id: String,
    pub output_dir: PathBuf,
    pub keep_temp_files: bool,
    pub fps: u32,
    pub intermediate_profile: IntermediateRecordingProfile,
    pub recording_video: VideoEncodeConfig,
    pub width: u32,
    pub height: u32,
    pub capture_origin_x: i32,
    pub capture_origin_y: i32,
    pub audio_tracks: Vec<AudioTrackManifest>,
    pub pause_intervals: Vec<PauseInterval>,
}

#[derive(Clone, Debug)]
pub struct LocalRecordingPaths {
    pub temp_dir: PathBuf,
    pub video_intermediate_path: PathBuf,
    pub video_index_path: PathBuf,
    pub mouse_path: PathBuf,
}

#[derive(Clone, Debug)]
pub struct RecordingArtifact {
    pub session_id: String,
    pub output_dir: PathBuf,
    pub local_paths: LocalRecordingPaths,
    pub bundle_path: PathBuf,
    pub audio_tracks: Vec<AudioTrackManifest>,
}

impl RecordingArtifact {
    pub fn load_manifest(&self) -> Result<SessionManifest> {
        self.read_embedded_manifest()
    }

    pub fn read_embedded_manifest(&self) -> Result<SessionManifest> {
        Ok(read_recording_bundle_footer(&self.bundle_path)?.manifest)
    }
}

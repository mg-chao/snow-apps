//! Version-2 editable bundles with a native SDR/HDR intermediate and separate
//! PCM tracks. The index is written incrementally, so recording memory is bounded.
use super::*;
use snow_audio_recorder::{
    AudioRecordingConfig, AudioRecordingSession, AudioTrackConfig, AudioTrackDevice,
};
use snow_recording_model::{
    BundleAssetKind, LocalRecordingPaths, RecordingArtifact, RecordingBundleAsset, SessionManifest,
};
use std::io::{BufWriter, Write};

pub struct NativeEditableSession {
    video: NativeRecordingSession,
    audio: Option<AudioRecordingSession>,
    directory: tempfile::TempDir,
    index: FrameIndexWriter,
    destination: PathBuf,
    codec: VideoCodec,
    cursor: Option<super::editable_cursor::EditableCursor>,
    cursor_mode: snow_media::CursorMode,
}
pub struct NativeEditableReport {
    pub artifact: RecordingArtifact,
    pub recording: NativeRecordingReport,
}
impl NativeEditableSession {
    /// `output_path` names the completed bundle. Separate cursor assets are
    /// spooled independently. Click/trail observations remain editable.
    pub fn start(mut config: NativeRecordingConfig) -> Result<Self> {
        if config.capture.cancellation.is_canceled() {
            return Err(native_error(MacError::Canceled));
        }
        if config
            .audio
            .as_ref()
            .is_some_and(|audio| audio.microphone.enabled && audio.microphone.required)
            && !snow_macos::microphone::microphone_authorized()
        {
            return Err(ScreenRecorderError::PermissionDenied(
                crate::MediaPermission::Microphone,
            ));
        }
        if config.effects.keyboard.is_some()
            || config.effects.record_mouse_clicks
            || config.effects.highlight_rgba[3] != 0
        {
            return Err(ScreenRecorderError::UnsupportedFeature(
                "editable input keycaps and mouse highlight assets are not yet supported".into(),
            ));
        }
        let destination = config.output_path.clone();
        let parent = destination
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(std::path::Path::new("."));
        std::fs::create_dir_all(parent)?;
        let directory = tempfile::Builder::new()
            .prefix(".snow-editable-")
            .tempdir_in(parent)?;
        let cursor_mode = config.capture.cursor;
        let separate = cursor_mode == snow_media::CursorMode::Separate;
        let cursor = if separate || config.effects.clicks || config.effects.trail {
            Some(super::editable_cursor::EditableCursor::new(
                directory.path(),
                separate,
                config.effects.clicks,
                config.effects.trail,
            )?)
        } else {
            None
        };
        if separate {
            config.capture.cursor = snow_media::CursorMode::Hidden;
        }
        // Input effects are retained as editable observations, not burned into
        // the intermediate. The video keeps its selected embedded/hidden cursor.
        config.effects = NativeEffectsConfig::default();
        config.output_path = directory.path().join("video.mp4");
        let audio_config = config.audio.take();
        let codec = config.codec;
        let index = FrameIndexWriter::new(directory.path().join("video.index"), config.fps)?;
        let video = NativeRecordingSession::start(config)?;
        // Exactly one system source: the video encoder is silent and audio is
        // retained separately for editing against the same pause-aware clock.
        let audio = audio_config
            .map(|audio| {
                let mut tracks = Vec::new();
                if audio.system.enabled {
                    let mut track = AudioTrackConfig::system_default("system");
                    track.required = audio.system.required;
                    tracks.push(track);
                }
                if audio.microphone.enabled {
                    let mut track = AudioTrackConfig::microphone_default("microphone");
                    track.required = audio.microphone.required;
                    if let snow_audio_recorder::DeviceSelector::Id(uid) = audio.microphone.device {
                        track.device = AudioTrackDevice::InputDeviceId(uid);
                    }
                    tracks.push(track);
                }
                if tracks.is_empty() {
                    return Ok(None);
                }
                AudioRecordingSession::start(
                    AudioRecordingConfig {
                        cancellation: video.config.capture.cancellation.clone(),
                        output_dir: directory.path().join("audio"),
                        tracks,
                        ..Default::default()
                    },
                    video.clock.clone(),
                )
                .map(Some)
            })
            .transpose()?
            .flatten();
        Ok(Self {
            video,
            audio,
            directory,
            index,
            destination,
            codec,
            cursor,
            cursor_mode,
        })
    }
    pub fn step(&mut self, timeout: Duration) -> Result<NativeRecordingEvent> {
        if let Some(cursor) = &mut self.cursor
            && let Some(reason) = cursor.sample(&self.video)?
        {
            let at_ms = self.video.clock.active_elapsed_ms(Instant::now());
            self.video.interruptions.push((at_ms, reason.clone()));
            self.video.media.discontinuities.push(
                snow_recording_model::media::TimelineDiscontinuity {
                    timestamp: snow_media::time::MediaTime {
                        value: i64::try_from(at_ms).map_err(|_| {
                            ScreenRecorderError::InvalidConfig("input timeline overflow".into())
                        })?,
                        timescale: 1000,
                        domain: snow_media::time::ClockDomain::Session,
                        epoch: 0,
                    },
                    duration: None,
                    reason: snow_recording_model::media::DiscontinuityReason::SourceInterrupted,
                },
            );
            return Ok(NativeRecordingEvent::Interruption { at_ms, reason });
        }
        let event = self.video.step(timeout)?;
        if let NativeRecordingEvent::Frame { pts } = event {
            self.index.push(pts)?;
        }
        Ok(event)
    }
    pub fn pause(&mut self) {
        self.video.pause();
        if let Some(audio) = &self.audio {
            audio.pause();
        }
    }
    pub fn resume(&mut self) {
        self.video.resume();
        if let Some(audio) = &self.audio {
            audio.resume();
        }
    }
    pub fn finish(self) -> Result<NativeEditableReport> {
        let cancellation = self.video.config.capture.cancellation.clone();
        let clock = self.video.clock.clone();
        let fps = self.video.config.fps;
        let output = self.video.config.output;
        let mut recording = self.video.finish()?;
        recording.media.cursor = self.cursor_mode;
        let end_ms = clock.active_elapsed_ms(Instant::now());
        self.index.finish(end_ms)?;
        let audio = self.audio.map(AudioRecordingSession::finish).transpose()?;
        let tracks = audio.map(|a| a.tracks).unwrap_or_default();
        recording.media.discontinuities.extend(
            tracks
                .iter()
                .flat_map(|track| track.discontinuities.iter().cloned()),
        );
        recording
            .media
            .validate()
            .map_err(ScreenRecorderError::InvalidConfig)?;
        let root = self.directory.path();
        let mouse = root.join("mouse.bin");
        if let Some(cursor) = self.cursor {
            cursor.finish(&mouse)?;
        } else {
            snow_recording_model::write_mouse_records(
                &mouse,
                &snow_recording_model::MouseStore::new(),
            )?;
        }
        let index = root.join("video.index");
        let mut assets = vec![
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
        ];
        for track in &tracks {
            assets.push(RecordingBundleAsset {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(&track.manifest.asset_id),
                path: &track.path,
            });
        }
        let output_dir = self
            .destination
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(std::path::Path::new("."))
            .to_path_buf();
        let manifest = SessionManifest {
            video_codec: self.codec,
            media: recording.media.clone(),
            session_id: uuid::Uuid::new_v4().to_string(),
            output_dir: output_dir.clone(),
            keep_temp_files: false,
            fps,
            intermediate_profile: snow_recording_model::IntermediateRecordingProfile::EditFast,
            recording_video: Default::default(),
            width: output.width,
            height: output.height,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: tracks.iter().map(|t| t.manifest.clone()).collect(),
            pause_intervals: clock
                .pause_intervals()
                .into_iter()
                .map(|p| snow_recording_model::PauseInterval {
                    start_ms: p.start_ms,
                    end_ms: p.end_ms,
                })
                .collect(),
        };
        let intermediate = root.join("video.mp4");
        snow_recording_model::write_recording_bundle(&intermediate, &manifest, &assets)?;
        cancellation
            .commit(|| std::fs::rename(&intermediate, &self.destination))
            .map_err(|_| native_error(MacError::Canceled))??;
        let artifact = RecordingArtifact {
            session_id: manifest.session_id,
            output_dir,
            local_paths: LocalRecordingPaths {
                temp_dir: PathBuf::new(),
                video_intermediate_path: self.destination.clone(),
                video_index_path: PathBuf::new(),
                mouse_path: PathBuf::new(),
            },
            bundle_path: self.destination,
            audio_tracks: manifest.audio_tracks,
        };
        // Embedded metadata supersedes the intermediate's temporary sidecar.
        recording.manifest_path = artifact.bundle_path.clone();
        Ok(NativeEditableReport {
            artifact,
            recording,
        })
    }
}
struct FrameIndexWriter {
    writer: BufWriter<std::fs::File>,
    fps: u32,
    count: u64,
    pending_ms: Option<u64>,
}
impl FrameIndexWriter {
    fn new(path: PathBuf, fps: u32) -> Result<Self> {
        if fps == 0 {
            return Err(ScreenRecorderError::InvalidConfig(
                "frame rate must be nonzero".into(),
            ));
        }
        let mut writer = BufWriter::new(std::fs::File::create(path)?);
        writer.write_all(b"SVIDX\0\0")?;
        Ok(Self {
            writer,
            fps,
            count: 0,
            pending_ms: None,
        })
    }
    fn write_pending(&mut self, end: u64) -> Result<()> {
        if let Some(start) = self.pending_ms {
            let duration = end
                .checked_sub(start)
                .filter(|d| *d > 0)
                .and_then(|d| u32::try_from(d).ok())
                .ok_or_else(|| {
                    ScreenRecorderError::InvalidConfig("invalid editable frame duration".into())
                })?;
            self.writer.write_all(&self.count.to_le_bytes())?;
            self.writer.write_all(&start.to_le_bytes())?;
            self.writer.write_all(&duration.to_le_bytes())?;
            self.count += 1;
        }
        Ok(())
    }
    fn push(&mut self, pts: u64) -> Result<()> {
        let ms = u64::try_from(u128::from(pts) * 1000 / u128::from(self.fps)).map_err(|_| {
            ScreenRecorderError::InvalidConfig("editable timestamp overflow".into())
        })?;
        self.write_pending(ms)?;
        self.pending_ms = Some(ms);
        Ok(())
    }
    fn finish(mut self, end: u64) -> Result<()> {
        if self.pending_ms.is_none() {
            return Err(ScreenRecorderError::InvalidConfig(
                "editable recording has no video frames".into(),
            ));
        }
        self.write_pending(end.max(self.pending_ms.unwrap().saturating_add(1)))?;
        self.writer.flush()?;
        Ok(())
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn incremental_index_preserves_dropped_frame_time_and_final_duration() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("index");
        let mut writer = FrameIndexWriter::new(path.clone(), 30).unwrap();
        writer.push(0).unwrap();
        writer.push(3).unwrap();
        writer.push(4).unwrap();
        writer.finish(200).unwrap();
        let bytes = std::fs::read(path).unwrap();
        let values: Vec<_> = bytes[7..]
            .chunks_exact(20)
            .map(|b| {
                (
                    u64::from_le_bytes(b[..8].try_into().unwrap()),
                    u64::from_le_bytes(b[8..16].try_into().unwrap()),
                    u32::from_le_bytes(b[16..20].try_into().unwrap()),
                )
            })
            .collect();
        assert_eq!(values, vec![(0, 0, 100), (1, 100, 33), (2, 133, 67)]);
    }
}

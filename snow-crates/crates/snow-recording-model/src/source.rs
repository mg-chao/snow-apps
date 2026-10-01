//! A common source description independent of version-specific bundle decoding.
use crate::{
    AudioTrackManifest, FinalizedTimeline, RecordingModelError, RenderConfig, RenderMetadata,
    SessionManifest, VideoCodec,
};

/// Source media geometry is separate from the requested final output canvas.
/// Version adapters supply timing without changing the positional v2 wire format.
#[derive(Clone, Debug)]
pub struct SourceDescription {
    pub media: crate::media::RecordedMedia,
    pub codec: VideoCodec,
    pub logical_width: u32,
    pub logical_height: u32,
    pub coded_width: u32,
    pub coded_height: u32,
    pub timeline: FinalizedTimeline,
    pub audio_tracks: Vec<AudioTrackManifest>,
    /// Legacy sources obtain editable effects from the export request instead.
    pub render: Option<RenderConfig>,
}

impl SourceDescription {
    pub fn legacy(manifest: &SessionManifest, timeline: FinalizedTimeline) -> crate::Result<Self> {
        Self::create(manifest, timeline, manifest.width, manifest.height, None)
    }

    pub fn deferred(manifest: &SessionManifest, metadata: &RenderMetadata) -> crate::Result<Self> {
        metadata.validate().map_err(RecordingModelError::Decode)?;
        if (metadata.render.output_width, metadata.render.output_height)
            != (manifest.width, manifest.height)
            || metadata.coded_width < manifest.width
            || metadata.coded_height < manifest.height
            || metadata.coded_width - manifest.width > 1
            || metadata.coded_height - manifest.height > 1
            || !metadata.coded_width.is_multiple_of(2)
            || !metadata.coded_height.is_multiple_of(2)
        {
            return Err(RecordingModelError::Decode(
                "clean source crop disagrees with its rendering metadata".into(),
            ));
        }
        Self::create(
            manifest,
            metadata.timeline,
            metadata.coded_width,
            metadata.coded_height,
            Some(metadata.render.clone()),
        )
    }

    fn create(
        manifest: &SessionManifest,
        timeline: FinalizedTimeline,
        coded_width: u32,
        coded_height: u32,
        render: Option<RenderConfig>,
    ) -> crate::Result<Self> {
        manifest
            .media
            .validate()
            .map_err(RecordingModelError::Decode)?;
        timeline.validate().map_err(RecordingModelError::Decode)?;
        if timeline.fps() != manifest.fps
            || manifest.width == 0
            || manifest.height == 0
            || manifest.width > 32768
            || manifest.height > 32768
        {
            return Err(RecordingModelError::Decode(
                "source dimensions or frame rate disagree with its timeline".into(),
            ));
        }
        Ok(Self {
            media: manifest.media.clone(),
            codec: manifest.video_codec,
            logical_width: manifest.width,
            logical_height: manifest.height,
            coded_width,
            coded_height,
            timeline,
            audio_tracks: manifest.audio_tracks.clone(),
            render,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest() -> SessionManifest {
        SessionManifest {
            media: crate::media::RecordedMedia::new(
                snow_media::ColorDescription::SRGB,
                snow_media::CursorMode::Separate,
                Vec::new(),
            ),
            video_codec: VideoCodec::H264,
            session_id: "source".into(),
            output_dir: Default::default(),
            keep_temp_files: false,
            fps: 30,
            intermediate_profile: crate::IntermediateRecordingProfile::EditFast,
            recording_video: Default::default(),
            width: 33,
            height: 25,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: Vec::new(),
            pause_intervals: Vec::new(),
        }
    }

    #[test]
    fn version_adapters_preserve_media_timing_and_logical_crop() {
        let manifest = manifest();
        let timeline = FinalizedTimeline::new(1001, manifest.fps).unwrap();
        let metadata = RenderMetadata {
            render: RenderConfig {
                output_width: 33,
                output_height: 25,
                output_fps: 30,
                effects: Default::default(),
                playback_overlay: Default::default(),
            },
            timeline,
            coded_width: 34,
            coded_height: 26,
        };
        let legacy = SourceDescription::legacy(&manifest, timeline).unwrap();
        let deferred = SourceDescription::deferred(&manifest, &metadata).unwrap();
        assert_eq!(legacy.timeline, deferred.timeline);
        assert_eq!((legacy.logical_width, legacy.logical_height), (33, 25));
        assert_eq!((deferred.coded_width, deferred.coded_height), (34, 26));
        assert!(legacy.render.is_none());
        assert_eq!(deferred.render, Some(metadata.render.clone()));
        assert!(
            SourceDescription::deferred(
                &manifest,
                &RenderMetadata {
                    coded_width: 36,
                    ..metadata.clone()
                }
            )
            .is_err()
        );
        assert!(
            SourceDescription::deferred(
                &manifest,
                &RenderMetadata {
                    coded_width: 33,
                    ..metadata
                }
            )
            .is_err()
        );
        assert!(
            SourceDescription::legacy(&manifest, FinalizedTimeline::new(1001, 24).unwrap())
                .is_err()
        );
    }
}

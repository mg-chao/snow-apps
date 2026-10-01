//! Portable rendering policy and an explicitly finalized, pause-free media timeline.
use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct KeyboardOverlayConfig {
    pub font: Option<KeyboardOverlayFont>,
    pub keycap_size: u32,
    pub background_rgba: [u8; 4],
    pub text_rgba: [u8; 4],
    pub border_rgba: [u8; 4],
    pub labels: BTreeMap<u16, String>,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct KeyboardOverlayFont {
    pub family: String,
    pub cjk_family: String,
    pub weight: u32,
}

impl KeyboardOverlayFont {
    pub fn new(family: &str, cjk_family: &str, weight: u32) -> Result<Self, String> {
        if !(1..=999).contains(&weight)
            || [family, cjk_family].iter().any(|name| {
                name.trim().is_empty() || name.len() > 256 || name.chars().any(char::is_control)
            })
        {
            return Err("invalid keyboard font family or weight".into());
        }
        Ok(Self {
            family: family.into(),
            cjk_family: cjk_family.into(),
            weight,
        })
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct EffectsConfig {
    pub show_cursor: bool,
    pub keyboard: Option<KeyboardOverlayConfig>,
    pub show_keyboard: bool,
    pub record_mouse_clicks: bool,
    pub mouse_trail_rgba: [u8; 4],
    pub mouse_trail_duration_ms: u64,
    pub mouse_click_rgba: [u8; 4],
    pub mouse_highlight_rgba: [u8; 4],
}

impl Default for EffectsConfig {
    fn default() -> Self {
        Self {
            show_cursor: true,
            keyboard: None,
            show_keyboard: false,
            record_mouse_clicks: false,
            mouse_trail_rgba: [0; 4],
            mouse_trail_duration_ms: 500,
            mouse_click_rgba: [0; 4],
            mouse_highlight_rgba: [0; 4],
        }
    }
}

impl EffectsConfig {
    pub fn validate(&self) -> Result<(), String> {
        if !(100..=2000).contains(&self.mouse_trail_duration_ms) {
            return Err("trail duration must be between 100 and 2000 ms".into());
        }
        if let Some(style) = &self.keyboard {
            if !(32..=128).contains(&style.keycap_size) {
                return Err("keycap size must be between 32 and 128 pixels".into());
            }
            if let Some(font) = &style.font {
                KeyboardOverlayFont::new(&font.family, &font.cjk_family, font.weight)?;
            }
            if style.labels.len() > 1024 || style.labels.values().any(|label| label.len() > 1024) {
                return Err("keyboard labels exceed the rendering limits".into());
            }
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Default, Serialize, Deserialize, PartialEq, Eq)]
pub enum PlaybackOverlay {
    #[default]
    None,
    ProgressBar {
        rgba: [u8; 4],
    },
    PlaybackTime {
        rgba: [u8; 4],
    },
}

impl PlaybackOverlay {
    pub fn enabled(self) -> bool {
        !matches!(self, Self::None)
    }
}

/// Pixel dimensions describe the final encoded canvas, never desktop coordinates.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RenderConfig {
    pub output_width: u32,
    pub output_height: u32,
    pub output_fps: u32,
    pub effects: EffectsConfig,
    pub playback_overlay: PlaybackOverlay,
}

impl RenderConfig {
    pub fn validate(&self) -> Result<(), String> {
        if [self.output_width, self.output_height]
            .into_iter()
            .any(|value| value == 0 || value > 32768)
        {
            return Err("render dimensions must be between 1 and 32768 pixels".into());
        }
        if self.output_fps == 0 || self.output_fps > 1000 {
            return Err("render frame rate must be between 1 and 1000".into());
        }
        self.effects.validate()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RenderFrame {
    pub index: u64,
    pub pts: u64,
    pub timestamp_ms: u64,
    pub progress: f64,
}

/// A compact rational schedule. Its memory use does not depend on duration or FPS.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct FinalizedTimeline {
    duration_ms: u64,
    fps: u32,
    frames: u64,
}

impl FinalizedTimeline {
    pub fn new(duration_ms: u64, fps: u32) -> Result<Self, String> {
        if duration_ms == 0 || fps == 0 || fps > 1000 {
            return Err(
                "finalized timeline requires a positive duration and FPS in 1..=1000".into(),
            );
        }
        let frames = (u128::from(duration_ms) * u128::from(fps)).div_ceil(1000);
        let frames = u64::try_from(frames)
            .map_err(|_| "finalized timeline frame count overflow".to_string())?;
        Ok(Self {
            duration_ms,
            fps,
            frames,
        })
    }

    pub fn duration_ms(self) -> u64 {
        self.duration_ms
    }

    pub fn fps(self) -> u32 {
        self.fps
    }

    pub fn frame_count(self) -> u64 {
        self.frames
    }

    pub fn frame(self, index: u64) -> Option<RenderFrame> {
        if index >= self.frames {
            return None;
        }
        Some(RenderFrame {
            index,
            pts: index,
            timestamp_ms: ((u128::from(index) * 1000) / u128::from(self.fps)) as u64,
            progress: if self.frames == 1 {
                1.0
            } else {
                index as f64 / (self.frames - 1) as f64
            },
        })
    }

    pub fn iter(self) -> TimelineFrames {
        TimelineFrames {
            timeline: self,
            next: 0,
        }
    }

    pub fn validate(self) -> Result<(), String> {
        if Self::new(self.duration_ms, self.fps)? != self {
            return Err("finalized timeline frame count disagrees with duration".into());
        }
        Ok(())
    }
}

/// Versioned extension metadata; the version-2 session manifest wire stays unchanged.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RenderMetadata {
    pub render: RenderConfig,
    pub timeline: FinalizedTimeline,
    pub coded_width: u32,
    pub coded_height: u32,
}

impl RenderMetadata {
    pub fn validate(&self) -> Result<(), String> {
        self.render.validate()?;
        self.timeline.validate()?;
        if self.timeline.fps() != self.render.output_fps {
            return Err("render frame rate disagrees with finalized timeline".into());
        }
        if [self.coded_width, self.coded_height]
            .into_iter()
            .any(|value| value == 0 || value > 32768)
        {
            return Err("coded source dimensions must be between 1 and 32768 pixels".into());
        }
        Ok(())
    }
}

const RENDER_METADATA_MAGIC: &[u8] = b"SNOWRENDER\0\x01";
const MAX_RENDER_METADATA_BYTES: u64 = 4 * 1024 * 1024;

pub fn write_render_metadata(
    path: &std::path::Path,
    metadata: &RenderMetadata,
) -> crate::Result<()> {
    use std::io::Write;
    metadata
        .validate()
        .map_err(crate::RecordingModelError::Decode)?;
    let mut output = std::io::BufWriter::new(std::fs::File::create(path)?);
    output.write_all(RENDER_METADATA_MAGIC)?;
    bincode::serialize_into(&mut output, metadata)
        .map_err(|error| crate::RecordingModelError::Decode(error.to_string()))?;
    output.flush()?;
    Ok(())
}

pub fn read_render_metadata(path: &std::path::Path) -> crate::Result<RenderMetadata> {
    decode_render_metadata(std::fs::File::open(path)?)
}

pub fn read_bundle_render_metadata(
    path: &std::path::Path,
) -> crate::Result<Option<RenderMetadata>> {
    use std::io::{Read, Seek};
    let footer = crate::read_recording_bundle_footer(path)?;
    let Some(asset) = footer.asset(crate::BundleAssetKind::RenderMetadata, None) else {
        return Ok(None);
    };
    if asset.len > MAX_RENDER_METADATA_BYTES {
        return Err(crate::RecordingModelError::Decode(
            "render metadata exceeds maximum size".into(),
        ));
    }
    let mut input = std::fs::File::open(path)?;
    input.seek(std::io::SeekFrom::Start(asset.offset))?;
    decode_render_metadata(input.take(asset.len)).map(Some)
}

fn decode_render_metadata(input: impl std::io::Read) -> crate::Result<RenderMetadata> {
    use bincode::Options;
    use std::io::Read;
    let mut input = std::io::BufReader::new(input);
    let mut magic = [0u8; 12];
    input.read_exact(&mut magic)?;
    if magic != RENDER_METADATA_MAGIC {
        return Err(crate::RecordingModelError::Decode(
            "unsupported render metadata format".into(),
        ));
    }
    let metadata: RenderMetadata = bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(MAX_RENDER_METADATA_BYTES)
        .deserialize_from(&mut input)
        .map_err(|error| crate::RecordingModelError::Decode(error.to_string()))?;
    metadata
        .validate()
        .map_err(crate::RecordingModelError::Decode)?;
    let mut trailing = [0];
    if input.read(&mut trailing)? != 0 {
        return Err(crate::RecordingModelError::Decode(
            "render metadata has trailing bytes".into(),
        ));
    }
    Ok(metadata)
}

pub struct TimelineFrames {
    timeline: FinalizedTimeline,
    next: u64,
}

impl Iterator for TimelineFrames {
    type Item = RenderFrame;
    fn next(&mut self) -> Option<Self::Item> {
        let frame = self.timeline.frame(self.next)?;
        self.next += 1;
        Some(frame)
    }
    fn size_hint(&self) -> (usize, Option<usize>) {
        match usize::try_from(self.timeline.frames - self.next) {
            Ok(count) => (count, Some(count)),
            Err(_) => (usize::MAX, None),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rational_timeline_is_compact_and_has_exact_endpoints() {
        for fps in [1, 24, 30, 60, 144, 1000] {
            let timeline = FinalizedTimeline::new(3_600_001, fps).unwrap();
            assert_eq!(timeline.frame_count(), u64::from(fps) * 3600 + 1);
            assert_eq!(timeline.frame(0).unwrap().progress, 0.0);
            assert_eq!(
                timeline.frame(u64::from(fps) * 3600).unwrap().timestamp_ms,
                3_600_000
            );
            assert_eq!(
                timeline.frame(timeline.frame_count() - 1).unwrap().progress,
                1.0
            );
            assert!(timeline.frame(timeline.frame_count()).is_none());
        }
        assert_eq!(
            FinalizedTimeline::new(1, 30)
                .unwrap()
                .frame(0)
                .unwrap()
                .progress,
            1.0
        );
        assert!(FinalizedTimeline::new(u64::MAX, 1000).is_ok());
        assert!(FinalizedTimeline::new(0, 30).is_err());
    }
    #[test]
    fn timeline_iteration_does_not_round_accumulate() {
        let frames: Vec<_> = FinalizedTimeline::new(1001, 30).unwrap().iter().collect();
        assert_eq!(frames.len(), 31);
        assert_eq!(frames[30].timestamp_ms, 1000);
        assert_eq!(frames[1].timestamp_ms, 33);
    }

    #[test]
    fn render_extension_preserves_captured_fonts_and_rejects_inconsistent_timeline() {
        let path = std::env::temp_dir().join(format!("snow-render-{}", uuid::Uuid::new_v4()));
        let mut metadata = RenderMetadata {
            render: RenderConfig {
                output_width: 1920,
                output_height: 1080,
                output_fps: 30,
                effects: EffectsConfig {
                    keyboard: Some(KeyboardOverlayConfig {
                        font: Some(
                            KeyboardOverlayFont::new("Captured font", "Captured Han", 500).unwrap(),
                        ),
                        keycap_size: 64,
                        background_rgba: [31, 31, 31, 204],
                        text_rgba: [255; 4],
                        border_rgba: [0; 4],
                        labels: BTreeMap::from([(0x20, "Space".into())]),
                    }),
                    ..EffectsConfig::default()
                },
                playback_overlay: PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            },
            timeline: FinalizedTimeline::new(125_000, 30).unwrap(),
            coded_width: 3840,
            coded_height: 2160,
        };
        write_render_metadata(&path, &metadata).unwrap();
        assert_eq!(read_render_metadata(&path).unwrap(), metadata);
        metadata.render.output_fps = 60;
        assert!(metadata.validate().is_err());
        let mut bytes = std::fs::read(&path).unwrap();
        bytes.push(0);
        std::fs::write(&path, bytes).unwrap();
        assert!(read_render_metadata(&path).is_err());
        std::fs::remove_file(path).unwrap();
    }
}

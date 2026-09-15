//! Versioned recording media semantics. Desktop geometry and pixel geometry
//! are distinct, and unavailable edits can be determined from cursor mode.
use serde::{Deserialize, Serialize};
use snow_media::{
    ColorDescription, CursorMode,
    geometry::{DesktopTransform, PixelRect},
    time::MediaTime,
};

pub const RECORDING_FORMAT_VERSION: u32 = 2;
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct GeometryChange {
    pub timestamp_ms: u64,
    pub generation: u64,
    pub transform: DesktopTransform,
    /// Rectangle occupied by the source in the encoded canvas (letterboxing is explicit).
    pub destination: PixelRect,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub enum DiscontinuityReason {
    SourceInterrupted,
    ClockReset,
    AudioOverflow,
    VideoDropped,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct TimelineDiscontinuity {
    pub timestamp: MediaTime,
    pub duration: Option<MediaTime>,
    pub reason: DiscontinuityReason,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RecordedMedia {
    pub format_version: u32,
    pub color: ColorDescription,
    pub cursor: CursorMode,
    pub geometry: Vec<GeometryChange>,
    pub discontinuities: Vec<TimelineDiscontinuity>,
}
impl RecordedMedia {
    pub fn new(color: ColorDescription, cursor: CursorMode, geometry: Vec<GeometryChange>) -> Self {
        Self {
            format_version: RECORDING_FORMAT_VERSION,
            color,
            cursor,
            geometry,
            discontinuities: Vec::new(),
        }
    }
    /// Standalone direct-recording metadata. Bundled editable recordings store
    /// the same structure inside SessionManifest instead.
    pub fn write_to(&self, path: &std::path::Path) -> crate::error::Result<()> {
        self.validate()
            .map_err(crate::error::RecordingModelError::Decode)?;
        let mut file = std::io::BufWriter::new(std::fs::File::create(path)?);
        use std::io::Write;
        file.write_all(b"SNOWMEDIA\0\x02")?;
        bincode::serialize_into(&mut file, self)
            .map_err(|e| crate::error::RecordingModelError::Decode(e.to_string()))?;
        file.flush()?;
        Ok(())
    }
    pub fn read_from(path: &std::path::Path) -> crate::error::Result<Self> {
        use bincode::Options;
        use std::io::Read;
        let mut file = std::io::BufReader::new(std::fs::File::open(path)?);
        let mut magic = [0; 11];
        file.read_exact(&mut magic)?;
        if &magic != b"SNOWMEDIA\0\x02" {
            return Err(crate::error::RecordingModelError::Decode(
                "unsupported media manifest format".into(),
            ));
        }
        let value: Self = bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_limit(64 * 1024 * 1024)
            .deserialize_from(file)
            .map_err(|e| crate::error::RecordingModelError::Decode(e.to_string()))?;
        value
            .validate()
            .map_err(crate::error::RecordingModelError::Decode)?;
        Ok(value)
    }
    pub fn validate(&self) -> Result<(), String> {
        if self.format_version != RECORDING_FORMAT_VERSION {
            return Err(format!(
                "unsupported recording format version {}; expected {RECORDING_FORMAT_VERSION}",
                self.format_version
            ));
        }
        let mut last = None;
        for item in &self.geometry {
            DesktopTransform::new(item.transform.source, item.transform.output)
                .map_err(|e| e.to_string())?;
            if item.destination.width == 0
                || item.destination.height == 0
                || item
                    .destination
                    .x
                    .checked_add(item.destination.width)
                    .is_none()
                || item
                    .destination
                    .y
                    .checked_add(item.destination.height)
                    .is_none()
            {
                return Err("invalid recording destination rectangle".into());
            }
            if last.is_some_and(|(time, generation)| {
                item.timestamp_ms < time || item.generation <= generation
            }) {
                return Err("recording geometry changes must have monotonic timestamps and increasing generations".into());
            }
            last = Some((item.timestamp_ms, item.generation));
        }
        for item in &self.discontinuities {
            if item.timestamp.timescale == 0
                || item
                    .duration
                    .is_some_and(|d| d.timescale == 0 || d.value < 0)
            {
                return Err("invalid recording discontinuity timestamp".into());
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_media::geometry::{DesktopRect, DesktopSpace, PixelSize};
    fn sample() -> RecordedMedia {
        RecordedMedia::new(
            ColorDescription::HDR10,
            CursorMode::Embedded,
            vec![GeometryChange {
                timestamp_ms: 0,
                generation: 1,
                destination: PixelRect {
                    x: 0,
                    y: 0,
                    width: 2880,
                    height: 1800,
                },
                transform: DesktopTransform::new(
                    DesktopRect {
                        space: DesktopSpace::Points,
                        x: -1440.0,
                        y: -200.0,
                        width: 1440.0,
                        height: 900.0,
                    },
                    PixelSize::new(2880, 1800).unwrap(),
                )
                .unwrap(),
            }],
        )
    }
    #[test]
    fn standalone_manifest_roundtrip() {
        let path =
            std::env::temp_dir().join(format!("snow-media-{}.snowmedia", uuid::Uuid::new_v4()));
        sample().write_to(&path).unwrap();
        let loaded = RecordedMedia::read_from(&path).unwrap();
        assert_eq!(loaded.color, ColorDescription::HDR10);
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn media_roundtrip_preserves_retina_color_and_cursor() {
        let media = sample();
        let bytes = bincode::serialize(&media).unwrap();
        let decoded: RecordedMedia = bincode::deserialize(&bytes).unwrap();
        decoded.validate().unwrap();
        assert_eq!(decoded.color, ColorDescription::HDR10);
        assert_eq!(decoded.cursor, CursorMode::Embedded);
        assert_eq!(decoded.geometry[0].transform, media.geometry[0].transform);
    }
    #[test]
    fn rejects_old_versions_invalid_geometry_and_unordered_changes() {
        let mut media = sample();
        media.format_version = 1;
        assert!(
            media
                .validate()
                .unwrap_err()
                .contains("unsupported recording format")
        );
        media.format_version = RECORDING_FORMAT_VERSION;
        media.geometry.push(media.geometry[0].clone());
        assert!(media.validate().is_err());
        media.geometry.pop();
        media.geometry[0].transform.source.x = f64::NAN;
        assert!(media.validate().is_err());
    }
}

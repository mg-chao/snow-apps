//! Incremental, versioned input observations. Replay holds one event rather than a recording.
use std::fs::{File, OpenOptions};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Take, Write};
use std::path::Path;

use bincode::Options;
use serde::{Deserialize, Serialize};

use crate::{
    BundleAssetKind, CursorFrameRecord, CursorShapeRecord, RecordingModelError, Result,
    read_recording_bundle_footer,
};

const INPUT_MAGIC: &[u8; 11] = b"SNOWINPUT\0\x03";
pub const MAX_INPUT_EVENT_BYTES: usize = 16 * 1024 * 1024;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct KeyEventRecord {
    pub at_ms: u64,
    pub key: u16,
    pub down: bool,
    pub label: String,
    pub modifiers: Vec<(u16, String)>,
}

/// Vertical wheel direction, independent of device-specific delta units.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
pub enum ScrollDirection {
    Up,
    Down,
}
impl ScrollDirection {
    pub fn from_delta(delta: f64) -> Option<Self> {
        if !delta.is_finite() || delta == 0.0 {
            None
        } else if delta > 0.0 {
            Some(Self::Up)
        } else {
            Some(Self::Down)
        }
    }
    pub fn key(self) -> u16 {
        match self {
            Self::Up => 0x205,
            Self::Down => 0x206,
        }
    }
    pub fn from_key(key: u16) -> Option<Self> {
        match key {
            0x205 => Some(Self::Up),
            0x206 => Some(Self::Down),
            _ => None,
        }
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::Up => "Scroll up",
            Self::Down => "Scroll down",
        }
    }
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum InputMouseButton {
    Left,
    Right,
    Middle,
    Button4,
    Button5,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct RecordedMouseClick {
    pub x: i32,
    pub y: i32,
    pub button: InputMouseButton,
    pub down: bool,
    /// Captured label/modifiers and original observation time, independent of replay locale.
    pub key: KeyEventRecord,
}

/// Affine reprojection of spatial input history when the capture viewport moves.
/// Coordinates are in the recorded input canvas; observation ages are unchanged.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq)]
pub struct PointerTransform {
    pub scale: [f64; 2],
    pub offset: [f64; 2],
}

impl PointerTransform {
    pub fn between(
        previous: snow_media::geometry::DesktopRect,
        previous_destination: snow_media::geometry::PixelRect,
        next: snow_media::geometry::DesktopRect,
        next_destination: snow_media::geometry::PixelRect,
    ) -> Result<Self> {
        previous
            .validate()
            .map_err(|_| invalid("invalid previous pointer viewport"))?;
        next.validate()
            .map_err(|_| invalid("invalid next pointer viewport"))?;
        if previous.space != next.space
            || previous_destination.width == 0
            || previous_destination.height == 0
            || next_destination.width == 0
            || next_destination.height == 0
        {
            return Err(invalid("incompatible pointer viewports"));
        }
        let scale = [
            previous.width / f64::from(previous_destination.width)
                * f64::from(next_destination.width)
                / next.width,
            previous.height / f64::from(previous_destination.height)
                * f64::from(next_destination.height)
                / next.height,
        ];
        let result = Self {
            scale,
            offset: [
                f64::from(next_destination.x)
                    + (previous.x - next.x) * f64::from(next_destination.width) / next.width
                    - f64::from(previous_destination.x) * scale[0],
                f64::from(next_destination.y)
                    + (previous.y - next.y) * f64::from(next_destination.height) / next.height
                    - f64::from(previous_destination.y) * scale[1],
            ],
        };
        result.validate()?;
        Ok(result)
    }

    pub fn validate(self) -> Result<()> {
        if self.scale.into_iter().any(|v| !v.is_finite() || v <= 0.0)
            || self.offset.into_iter().any(|v| !v.is_finite())
        {
            return Err(invalid("invalid pointer viewport transform"));
        }
        Ok(())
    }

    pub fn project(self, point: (i32, i32)) -> (i32, i32) {
        (
            (f64::from(point.0) * self.scale[0] + self.offset[0]).round() as i32,
            (f64::from(point.1) * self.scale[1] + self.offset[1]).round() as i32,
        )
    }

    pub fn for_output(self, source: (u32, u32), output: (u32, u32)) -> Self {
        Self {
            scale: self.scale,
            offset: [
                self.offset[0] * f64::from(output.0) / f64::from(source.0.max(1)),
                self.offset[1] * f64::from(output.1) / f64::from(source.1.max(1)),
            ],
        }
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum RecordedInput {
    CursorShape(CursorShapeRecord),
    Cursor(CursorFrameRecord),
    Click(RecordedMouseClick),
    Key(KeyEventRecord),
    /// Break trails and release held input on pause/resume or capture geometry changes.
    Reset,
    /// The sole trail stream: accepted output-slot positions, never raw polling events.
    Pointer {
        position: Option<(i32, i32)>,
        continuity: u64,
        /// Original pause-free observation time; admission may occur on a later output slot.
        at_ms: u64,
    },
    /// Release keys after input-observer overflow/interruption while existing mouse effects fade.
    KeyboardReset,
    /// Vertical wheel observation; key carries direction, localized label and original time.
    Scroll(KeyEventRecord),
    /// Reproject existing mouse effects through a moving viewport, without resetting keys.
    PointerViewport(PointerTransform),
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct RecordedInputEvent {
    /// Pause-free admission time. Original observation times remain in the payload.
    pub timestamp_ms: u64,
    /// Strictly increasing admission order, including observations at the same timestamp.
    pub sequence: u64,
    pub event: RecordedInput,
}

impl RecordedInputEvent {
    pub fn validate(&self) -> Result<()> {
        match &self.event {
            RecordedInput::PointerViewport(transform) => transform.validate()?,
            RecordedInput::CursorShape(shape) => {
                let expected = u64::from(shape.width) * u64::from(shape.height) * 4;
                if shape.width == 0
                    || shape.height == 0
                    || expected > MAX_INPUT_EVENT_BYTES as u64
                    || shape.shape_rgba.len() as u64 != expected
                    || shape.hotspot_x >= shape.width
                    || shape.hotspot_y >= shape.height
                {
                    return Err(invalid("invalid input cursor shape"));
                }
            }
            RecordedInput::Key(key) => validate_key(key)?,
            RecordedInput::Scroll(key) => {
                validate_key(key)?;
                if ScrollDirection::from_key(key.key).is_none()
                    || !key.down
                    || !key.modifiers.is_empty()
                {
                    return Err(invalid("invalid scroll observation"));
                }
            }
            RecordedInput::Click(click) => {
                validate_key(&click.key)?;
                if click.down != click.key.down {
                    return Err(invalid("mouse click and key state disagree"));
                }
            }
            _ => {}
        }
        Ok(())
    }
}

fn validate_key(key: &KeyEventRecord) -> Result<()> {
    if key.label.len() > 1024
        || key.modifiers.len() > 16
        || key.modifiers.iter().any(|(_, label)| label.len() > 1024)
    {
        return Err(invalid("input key labels exceed rendering limits"));
    }
    Ok(())
}

fn invalid(message: &str) -> RecordingModelError {
    RecordingModelError::Decode(message.into())
}

fn validate_order(previous: Option<(u64, u64)>, event: &RecordedInputEvent) -> Result<()> {
    if previous.is_some_and(|(timestamp, sequence)| {
        event.timestamp_ms < timestamp || event.sequence <= sequence
    }) {
        return Err(invalid(
            "input admission timestamps and sequence must be monotonic",
        ));
    }
    event.validate()
}

pub struct InputStoreWriter {
    output: BufWriter<File>,
    scratch: Vec<u8>,
    last: Option<(u64, u64)>,
}

impl InputStoreWriter {
    pub fn new(path: &Path) -> Result<Self> {
        let mut output = BufWriter::with_capacity(
            64 * 1024,
            OpenOptions::new().create_new(true).write(true).open(path)?,
        );
        output.write_all(INPUT_MAGIC)?;
        Ok(Self {
            output,
            scratch: Vec::new(),
            last: None,
        })
    }

    pub fn push(&mut self, event: &RecordedInputEvent) -> Result<()> {
        validate_order(self.last, event)?;
        self.scratch.clear();
        bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_limit(MAX_INPUT_EVENT_BYTES as u64)
            .serialize_into(&mut self.scratch, event)
            .map_err(|error| invalid(&format!("failed to encode input event: {error}")))?;
        if self.scratch.len() > MAX_INPUT_EVENT_BYTES {
            return Err(invalid("input event exceeds maximum size"));
        }
        self.output
            .write_all(&(self.scratch.len() as u32).to_le_bytes())?;
        self.output.write_all(&self.scratch)?;
        self.last = Some((event.timestamp_ms, event.sequence));
        Ok(())
    }

    pub fn finish(mut self) -> Result<()> {
        // A terminator detects truncation even exactly between two records.
        self.output.write_all(&0u32.to_le_bytes())?;
        self.output.flush()?;
        Ok(())
    }
}

pub struct InputStoreReader {
    input: BufReader<Take<File>>,
    scratch: Vec<u8>,
    pending: Option<RecordedInputEvent>,
    last: Option<(u64, u64)>,
    complete: bool,
}

impl InputStoreReader {
    pub fn open(path: &Path) -> Result<Self> {
        let input = File::open(path)?;
        let len = input.metadata()?.len();
        Self::from_range(input, 0, len)
    }

    pub fn from_bundle(path: &Path) -> Result<Option<Self>> {
        let footer = read_recording_bundle_footer(path)?;
        let Some(asset) = footer.asset(BundleAssetKind::InputEvents, None) else {
            return Ok(None);
        };
        Self::from_range(File::open(path)?, asset.offset, asset.len).map(Some)
    }

    fn from_range(mut file: File, offset: u64, len: u64) -> Result<Self> {
        file.seek(SeekFrom::Start(offset))?;
        let mut input = BufReader::with_capacity(64 * 1024, file.take(len));
        let mut magic = [0u8; INPUT_MAGIC.len()];
        input.read_exact(&mut magic)?;
        if &magic != INPUT_MAGIC && &magic != b"SNOWINPUT\0\x02" && &magic != b"SNOWINPUT\0\x01" {
            return Err(invalid("unsupported input store format"));
        }
        Ok(Self {
            input,
            scratch: Vec::new(),
            pending: None,
            last: None,
            complete: false,
        })
    }

    pub fn next_event(&mut self) -> Result<Option<RecordedInputEvent>> {
        if self.pending.is_some() {
            return Ok(self.pending.take());
        }
        if self.complete {
            return Ok(None);
        }
        let mut length = [0u8; 4];
        self.input.read_exact(&mut length)?;
        let length = u32::from_le_bytes(length) as usize;
        if length == 0 {
            let mut trailing = [0];
            if self.input.read(&mut trailing)? != 0 {
                return Err(invalid("input store has trailing bytes"));
            }
            self.complete = true;
            return Ok(None);
        }
        if length > MAX_INPUT_EVENT_BYTES {
            return Err(invalid("input event exceeds maximum size"));
        }
        self.scratch.resize(length, 0);
        self.input.read_exact(&mut self.scratch)?;
        let event: RecordedInputEvent = bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_limit(length as u64)
            .reject_trailing_bytes()
            .deserialize(&self.scratch)
            .map_err(|error| invalid(&format!("failed to decode input event: {error}")))?;
        validate_order(self.last, &event)?;
        self.last = Some((event.timestamp_ms, event.sequence));
        Ok(Some(event))
    }

    pub fn replay_until(
        &mut self,
        timestamp_ms: u64,
        mut visit: impl FnMut(RecordedInputEvent) -> Result<()>,
    ) -> Result<()> {
        while let Some(event) = self.next_event()? {
            if event.timestamp_ms > timestamp_ms {
                self.pending = Some(event);
                break;
            }
            visit(event)?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn follow_mouse_viewport_roundtrips_and_rejects_invalid_transforms() {
        let path = path();
        let transform = PointerTransform {
            scale: [2.0, 2.0],
            offset: [-400.0, 100.0],
        };
        assert_eq!(transform.project((210, -40)), (20, 20));
        let event = RecordedInputEvent {
            timestamp_ms: 32,
            sequence: 1,
            event: RecordedInput::PointerViewport(transform),
        };
        let mut writer = InputStoreWriter::new(&path).unwrap();
        writer.push(&event).unwrap();
        writer.finish().unwrap();
        assert_eq!(
            InputStoreReader::open(&path).unwrap().next_event().unwrap(),
            Some(event)
        );
        std::fs::remove_file(path).unwrap();
        for transform in [
            PointerTransform {
                scale: [0.0, 1.0],
                offset: [0.0; 2],
            },
            PointerTransform {
                scale: [1.0; 2],
                offset: [f64::NAN, 0.0],
            },
        ] {
            assert!(transform.validate().is_err());
        }
    }

    #[test]
    fn follow_mouse_viewport_projects_through_desktop_and_padded_canvas() {
        use snow_media::geometry::{DesktopRect, DesktopSpace, PixelRect};
        let previous = DesktopRect {
            space: DesktopSpace::PhysicalPixels,
            x: -1000.0,
            y: 100.0,
            width: 800.0,
            height: 600.0,
        };
        let next = DesktopRect {
            x: -600.0,
            y: 200.0,
            width: 400.0,
            height: 300.0,
            ..previous
        };
        let rect = PixelRect {
            x: 10,
            y: 20,
            width: 800,
            height: 600,
        };
        let transform = PointerTransform::between(previous, rect, next, rect).unwrap();
        // Old point (-500,250) becomes (200,100) in the new source, plus padding.
        assert_eq!(transform.project((510, 170)), (210, 120));
        let inverse = PointerTransform::between(next, rect, previous, rect).unwrap();
        assert_eq!(inverse.project(transform.project((510, 170))), (510, 170));
    }

    #[test]
    fn scroll_records_roundtrip_and_legacy_input_remains_readable() {
        let path = path();
        let scroll = RecordedInputEvent {
            timestamp_ms: 20,
            sequence: 0,
            event: RecordedInput::Scroll(KeyEventRecord {
                at_ms: 17,
                key: ScrollDirection::Down.key(),
                down: true,
                label: "Localized down".into(),
                modifiers: vec![],
            }),
        };
        let mut writer = InputStoreWriter::new(&path).unwrap();
        writer.push(&scroll).unwrap();
        writer.finish().unwrap();
        assert_eq!(
            InputStoreReader::open(&path).unwrap().next_event().unwrap(),
            Some(scroll)
        );
        std::fs::remove_file(&path).unwrap();
        let event = RecordedInputEvent {
            timestamp_ms: 0,
            sequence: 0,
            event: RecordedInput::Reset,
        };
        let mut writer = InputStoreWriter::new(&path).unwrap();
        writer.push(&event).unwrap();
        writer.finish().unwrap();
        let mut bytes = std::fs::read(&path).unwrap();
        bytes[10] = 1;
        std::fs::write(&path, bytes).unwrap();
        assert_eq!(
            InputStoreReader::open(&path).unwrap().next_event().unwrap(),
            Some(event)
        );
        std::fs::remove_file(path).unwrap();
        assert_eq!(
            ScrollDirection::from_delta(0.001),
            Some(ScrollDirection::Up)
        );
        assert_eq!(
            ScrollDirection::from_delta(-0.001),
            Some(ScrollDirection::Down)
        );
        assert_eq!(ScrollDirection::from_delta(0.0), None);
        assert_eq!(ScrollDirection::from_delta(f64::NAN), None);
    }
    fn path() -> std::path::PathBuf {
        std::env::temp_dir().join(format!("snow-input-{}", uuid::Uuid::new_v4()))
    }
    #[test]
    fn input_stream_preserves_equal_time_admission_order_and_future_events() {
        let path = path();
        let mut writer = InputStoreWriter::new(&path).unwrap();
        for sequence in 0..1000 {
            writer
                .push(&RecordedInputEvent {
                    timestamp_ms: sequence / 2,
                    sequence,
                    event: RecordedInput::Reset,
                })
                .unwrap();
        }
        writer.finish().unwrap();
        let mut reader = InputStoreReader::open(&path).unwrap();
        let mut seen = Vec::new();
        reader
            .replay_until(1, |event| {
                seen.push(event.sequence);
                Ok(())
            })
            .unwrap();
        assert_eq!(seen, vec![0, 1, 2, 3]);
        assert_eq!(reader.next_event().unwrap().unwrap().sequence, 4);
        reader.replay_until(u64::MAX, |_| Ok(())).unwrap();
        assert!(reader.next_event().unwrap().is_none());
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn stream_rejects_order_errors_and_truncation_at_a_record_boundary() {
        let path = path();
        let mut writer = InputStoreWriter::new(&path).unwrap();
        let event = RecordedInputEvent {
            timestamp_ms: 1,
            sequence: 1,
            event: RecordedInput::Reset,
        };
        writer.push(&event).unwrap();
        assert!(writer.push(&event).is_err());
        writer.finish().unwrap();
        let mut bytes = std::fs::read(&path).unwrap();
        bytes.truncate(bytes.len() - 4);
        std::fs::write(&path, bytes).unwrap();
        let mut reader = InputStoreReader::open(&path).unwrap();
        assert!(reader.next_event().unwrap().is_some());
        assert!(reader.next_event().is_err());
        std::fs::remove_file(path).unwrap();
    }
    #[test]
    fn corrupt_record_length_is_rejected_before_allocation() {
        let path = path();
        let mut bytes = INPUT_MAGIC.to_vec();
        bytes.extend_from_slice(&u32::MAX.to_le_bytes());
        std::fs::write(&path, bytes).unwrap();
        let mut reader = InputStoreReader::open(&path).unwrap();
        assert!(reader.next_event().is_err());
        assert!(reader.scratch.is_empty());
        std::fs::remove_file(path).unwrap();
    }
}

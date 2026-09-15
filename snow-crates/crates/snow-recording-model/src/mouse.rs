use std::fs::File;
use std::io::{BufReader, BufWriter, Write};
use std::path::Path;

use serde::{Deserialize, Serialize};

use crate::error::{RecordingModelError, Result};

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
    Middle,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub enum CursorShapeCompositionMode {
    AlphaBlend,
    MaskedColor,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CursorShapeRecord {
    pub shape_id: u64,
    pub hotspot_x: u32,
    pub hotspot_y: u32,
    pub width: u32,
    pub height: u32,
    pub mode: CursorShapeCompositionMode,
    pub shape_rgba: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct CursorFrameRecord {
    pub timestamp_ms: u64,
    pub x: i32,
    pub y: i32,
    pub visible: bool,
    pub shape_id: Option<u64>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ClickEventRecord {
    pub timestamp_ms: u64,
    pub x: i32,
    pub y: i32,
    pub button: MouseButton,
    pub down: bool,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct MouseStore {
    pub cursor_shapes: Vec<CursorShapeRecord>,
    pub cursor_frames: Vec<CursorFrameRecord>,
    pub clicks: Vec<ClickEventRecord>,
}

impl MouseStore {
    pub fn new() -> Self {
        Self::default()
    }
}

pub fn write_mouse_records(path: &Path, store: &MouseStore) -> Result<()> {
    let file = File::create(path)?;
    let mut writer = BufWriter::with_capacity(128 * 1024, file);
    bincode::serialize_into(&mut writer, store)
        .map_err(|err| RecordingModelError::Io(std::io::Error::other(err)))?;
    writer.flush()?;
    Ok(())
}

pub fn read_mouse_records(path: &Path) -> Result<MouseStore> {
    let file = File::open(path)?;
    bincode::deserialize_from(BufReader::with_capacity(128 * 1024, file))
        .map_err(|err| RecordingModelError::Decode(format!("failed to decode mouse store: {err}")))
}

pub fn decode_mouse_records(bytes: &[u8]) -> Result<MouseStore> {
    bincode::deserialize(bytes)
        .map_err(|err| RecordingModelError::Decode(format!("failed to decode mouse store: {err}")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mouse_store_roundtrip() {
        let path = std::env::temp_dir().join(format!(
            "snow-recording-model-mouse-store-{}.bin",
            uuid::Uuid::new_v4().simple()
        ));

        let mut store = MouseStore::new();
        store.cursor_frames.push(CursorFrameRecord {
            timestamp_ms: 10,
            x: 20,
            y: 30,
            visible: true,
            shape_id: None,
        });

        write_mouse_records(&path, &store).expect("write should succeed");
        let decoded = read_mouse_records(&path).expect("read should succeed");
        assert_eq!(decoded.cursor_frames.len(), 1);

        let _ = std::fs::remove_file(path);
    }
}

/// Incremental mouse assets. Use a private recording directory; memory does not
/// grow with recording length. The finished bytes match `MouseStore` exactly.
pub struct MouseStoreWriter {
    shapes: MouseSection,
    frames: MouseSection,
    clicks: MouseSection,
}
struct MouseSection {
    file: BufWriter<File>,
    path: std::path::PathBuf,
    count: u64,
}
impl MouseSection {
    fn new(directory: &Path, name: &str) -> Result<Self> {
        let path = directory.join(name);
        Ok(Self {
            file: BufWriter::new(
                std::fs::OpenOptions::new()
                    .read(true)
                    .write(true)
                    .create_new(true)
                    .open(&path)?,
            ),
            path,
            count: 0,
        })
    }
    fn push(&mut self, value: &impl Serialize) -> Result<()> {
        let next = self
            .count
            .checked_add(1)
            .ok_or_else(|| RecordingModelError::Decode("mouse record count overflow".into()))?;
        bincode::serialize_into(&mut self.file, value)
            .map_err(|error| RecordingModelError::Io(std::io::Error::other(error)))?;
        self.count = next;
        Ok(())
    }
    fn finish(mut self, output: &mut impl std::io::Write) -> Result<()> {
        use std::io::{Seek, Write};
        self.file.flush()?;
        self.file.get_mut().rewind()?;
        output.write_all(&self.count.to_le_bytes())?;
        std::io::copy(self.file.get_mut(), output)?;
        drop(self.file);
        std::fs::remove_file(self.path)?;
        Ok(())
    }
}
impl MouseStoreWriter {
    pub fn new(directory: &Path) -> Result<Self> {
        Ok(Self {
            shapes: MouseSection::new(directory, ".mouse-shapes")?,
            frames: MouseSection::new(directory, ".mouse-frames")?,
            clicks: MouseSection::new(directory, ".mouse-clicks")?,
        })
    }
    pub fn shape(&mut self, record: &CursorShapeRecord) -> Result<()> {
        self.shapes.push(record)
    }
    pub fn frame(&mut self, record: &CursorFrameRecord) -> Result<()> {
        self.frames.push(record)
    }
    pub fn click(&mut self, record: &ClickEventRecord) -> Result<()> {
        self.clicks.push(record)
    }
    pub fn finish(self, path: &Path) -> Result<()> {
        use std::io::Write;
        let mut output = BufWriter::new(File::create(path)?);
        self.shapes.finish(&mut output)?;
        self.frames.finish(&mut output)?;
        self.clicks.finish(&mut output)?;
        output.flush()?;
        Ok(())
    }
}

#[cfg(test)]
mod incremental_tests {
    use super::*;
    #[test]
    fn incremental_mouse_assets_match_the_in_memory_format() {
        let directory = std::env::temp_dir().join(format!("snow-mouse-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir(&directory).unwrap();
        let mut writer = MouseStoreWriter::new(&directory).unwrap();
        let mut store = MouseStore::new();
        store.cursor_shapes.push(CursorShapeRecord {
            shape_id: 1,
            hotspot_x: 0,
            hotspot_y: 0,
            width: 1,
            height: 1,
            mode: CursorShapeCompositionMode::AlphaBlend,
            shape_rgba: vec![255, 0, 0, 128],
        });
        writer.shape(&store.cursor_shapes[0]).unwrap();
        for timestamp_ms in 0..1000 {
            let frame = CursorFrameRecord {
                timestamp_ms,
                x: -20,
                y: 10,
                visible: true,
                shape_id: Some(1),
            };
            writer.frame(&frame).unwrap();
            store.cursor_frames.push(frame);
        }
        store.clicks.push(ClickEventRecord {
            timestamp_ms: 500,
            x: 0,
            y: 20,
            button: MouseButton::Left,
            down: true,
        });
        writer.click(&store.clicks[0]).unwrap();
        let path = directory.join("mouse.bin");
        writer.finish(&path).unwrap();
        assert_eq!(
            std::fs::read(&path).unwrap(),
            bincode::serialize(&store).unwrap()
        );
        assert_eq!(read_mouse_records(&path).unwrap().cursor_frames.len(), 1000);
        std::fs::remove_dir_all(directory).unwrap();
    }
}

//! Bounded reader for the original video admission timeline embedded in a bundle.
use std::fs::File;
use std::io::{BufReader, Read, Seek, SeekFrom, Take};
use std::path::Path;

use crate::{BundleAssetKind, RecordingModelError, Result, read_recording_bundle_footer};

pub const VIDEO_INDEX_MAGIC: &[u8] = b"SVIDX\0\0";
pub const VIDEO_INDEX_RECORD_BYTES: usize = 20;
const READ_BUFFER_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VideoIndexEntry {
    pub index: u64,
    pub timestamp_ms: u64,
    pub duration_ms: u32,
}

pub struct VideoIndexReader {
    input: BufReader<Take<File>>,
    remaining: u64,
    next_index: u64,
    last_timestamp: Option<u64>,
    records_offset: u64,
    record_count: u64,
}

impl VideoIndexReader {
    pub fn from_bundle(path: &Path) -> Result<Option<Self>> {
        let footer = read_recording_bundle_footer(path)?;
        let Some(asset) = footer.asset(BundleAssetKind::VideoIndex, None) else {
            return Ok(None);
        };
        Self::from_range(File::open(path)?, asset.offset, asset.len).map(Some)
    }

    fn from_range(mut file: File, offset: u64, length: u64) -> Result<Self> {
        let records = length
            .checked_sub(VIDEO_INDEX_MAGIC.len() as u64)
            .filter(|bytes| *bytes != 0 && *bytes % VIDEO_INDEX_RECORD_BYTES as u64 == 0)
            .ok_or_else(|| invalid("video index has an invalid or empty record range"))?;
        file.seek(SeekFrom::Start(offset))?;
        let mut input = BufReader::with_capacity(READ_BUFFER_BYTES, file.take(length));
        let mut magic = [0; VIDEO_INDEX_MAGIC.len()];
        input.read_exact(&mut magic)?;
        if magic != VIDEO_INDEX_MAGIC {
            return Err(invalid("video index has an unsupported header"));
        }
        Ok(Self {
            input,
            remaining: records / VIDEO_INDEX_RECORD_BYTES as u64,
            next_index: 0,
            last_timestamp: None,
            records_offset: offset + VIDEO_INDEX_MAGIC.len() as u64,
            record_count: records / VIDEO_INDEX_RECORD_BYTES as u64,
        })
    }

    pub fn remaining(&self) -> u64 {
        self.remaining
    }

    /// Position at the last admitted image at or before a source timestamp.
    /// Fixed-size records permit logarithmic disk reads with bounded buffering.
    pub fn seek_before(&mut self, timestamp_ms: u64) -> Result<()> {
        let mut file = self.input.get_ref().get_ref().try_clone()?;
        let mut low = 0;
        let mut high = self.record_count;
        while low < high {
            let middle = low + (high - low) / 2;
            file.seek(SeekFrom::Start(
                self.records_offset + middle * VIDEO_INDEX_RECORD_BYTES as u64 + 8,
            ))?;
            let mut bytes = [0; 8];
            file.read_exact(&mut bytes)?;
            if u64::from_le_bytes(bytes) <= timestamp_ms {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        self.next_index = low.saturating_sub(1);
        self.remaining = self.record_count - self.next_index;
        self.last_timestamp = None;
        file.seek(SeekFrom::Start(
            self.records_offset + self.next_index * VIDEO_INDEX_RECORD_BYTES as u64,
        ))?;
        self.input = BufReader::with_capacity(
            READ_BUFFER_BYTES,
            file.take(self.remaining * VIDEO_INDEX_RECORD_BYTES as u64),
        );
        Ok(())
    }

    pub fn next_frame(&mut self) -> Result<Option<VideoIndexEntry>> {
        if self.remaining == 0 {
            return Ok(None);
        }
        let mut record = [0; VIDEO_INDEX_RECORD_BYTES];
        self.input.read_exact(&mut record)?;
        let entry = VideoIndexEntry {
            index: u64::from_le_bytes(record[..8].try_into().expect("video index field")),
            timestamp_ms: u64::from_le_bytes(
                record[8..16].try_into().expect("video index timestamp"),
            ),
            duration_ms: u32::from_le_bytes(record[16..].try_into().expect("video index duration")),
        };
        if entry.index != self.next_index {
            return Err(invalid("video index frame sequence is not contiguous"));
        }
        if self
            .last_timestamp
            .is_some_and(|last| entry.timestamp_ms <= last)
        {
            return Err(invalid("video index timestamps must increase"));
        }
        if entry.duration_ms == 0 {
            return Err(invalid("video index frame duration must be positive"));
        }
        self.remaining -= 1;
        self.next_index += 1;
        self.last_timestamp = Some(entry.timestamp_ms);
        Ok(Some(entry))
    }
}

fn invalid(message: &str) -> RecordingModelError {
    RecordingModelError::Decode(message.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::path::PathBuf;

    fn path() -> PathBuf {
        std::env::temp_dir().join(format!("snow-video-index-{}", uuid::Uuid::new_v4()))
    }

    fn bytes(entries: &[VideoIndexEntry]) -> Vec<u8> {
        let mut bytes = VIDEO_INDEX_MAGIC.to_vec();
        for entry in entries {
            bytes.extend_from_slice(&entry.index.to_le_bytes());
            bytes.extend_from_slice(&entry.timestamp_ms.to_le_bytes());
            bytes.extend_from_slice(&entry.duration_ms.to_le_bytes());
        }
        bytes
    }

    fn reader(bytes: &[u8], length: u64) -> Result<VideoIndexReader> {
        let path = path();
        std::fs::write(&path, bytes).unwrap();
        let file = File::open(&path).unwrap();
        let result = VideoIndexReader::from_range(file, 0, length);
        std::fs::remove_file(path).unwrap();
        result
    }

    #[test]
    fn video_index_preserves_absolute_admission_and_stays_inside_its_asset() {
        let bundle = path();
        let index = path();
        let next_asset = path();
        let entries = [
            VideoIndexEntry {
                index: 0,
                timestamp_ms: 166,
                duration_ms: 1234,
            },
            VideoIndexEntry {
                index: 1,
                timestamp_ms: 1400,
                duration_ms: 633,
            },
            VideoIndexEntry {
                index: 2,
                timestamp_ms: 2033,
                duration_ms: 467,
            },
        ];
        std::fs::write(&bundle, b"video payload").unwrap();
        std::fs::write(&index, bytes(&entries)).unwrap();
        std::fs::write(&next_asset, [99; VIDEO_INDEX_RECORD_BYTES]).unwrap();
        let manifest = crate::SessionManifest {
            media: crate::media::RecordedMedia::new(
                snow_media::ColorDescription::SRGB,
                snow_media::CursorMode::Separate,
                Vec::new(),
            ),
            video_codec: crate::VideoCodec::H264,
            session_id: "index".into(),
            output_dir: Default::default(),
            keep_temp_files: false,
            fps: 30,
            intermediate_profile: crate::IntermediateRecordingProfile::EditFast,
            recording_video: Default::default(),
            width: 32,
            height: 24,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: Vec::new(),
            pause_intervals: Vec::new(),
        };
        assert!(VideoIndexReader::from_bundle(&bundle).is_err());
        crate::write_recording_bundle(&bundle, &manifest, &[]).unwrap();
        assert!(VideoIndexReader::from_bundle(&bundle).unwrap().is_none());
        std::fs::write(&bundle, b"video payload").unwrap();
        crate::write_recording_bundle(
            &bundle,
            &manifest,
            &[
                crate::RecordingBundleAsset {
                    kind: BundleAssetKind::VideoIndex,
                    asset_id: None,
                    path: &index,
                },
                crate::RecordingBundleAsset {
                    kind: BundleAssetKind::MouseStore,
                    asset_id: None,
                    path: &next_asset,
                },
            ],
        )
        .unwrap();
        let mut reader = VideoIndexReader::from_bundle(&bundle).unwrap().unwrap();
        for (position, expected) in entries.into_iter().enumerate() {
            assert_eq!(reader.remaining(), (entries.len() - position) as u64);
            assert_eq!(reader.next_frame().unwrap(), Some(expected));
        }
        assert_eq!(reader.remaining(), 0);
        assert_eq!(reader.next_frame().unwrap(), None);
        assert_eq!(reader.next_frame().unwrap(), None);
        drop(reader);
        for path in [bundle, index, next_asset] {
            std::fs::remove_file(path).unwrap();
        }
    }

    #[test]
    fn indexed_seeks_retain_the_containing_image_and_never_read_other_assets() {
        let entries = [166, 1400, 2033].map(|at| VideoIndexEntry {
            index: 0,
            timestamp_ms: at,
            duration_ms: 1,
        });
        let entries: Vec<_> = entries
            .into_iter()
            .enumerate()
            .map(|(i, mut entry)| {
                entry.index = i as u64;
                entry
            })
            .collect();
        let data = bytes(&entries);
        let mut reader = reader(&data, data.len() as u64).unwrap();
        for (time, expected) in [
            (0, 0),
            (166, 0),
            (1399, 0),
            (1400, 1),
            (99999, 2),
            (1700, 1),
        ] {
            reader.seek_before(time).unwrap();
            assert_eq!(reader.next_frame().unwrap(), Some(entries[expected]));
        }
    }

    #[test]
    fn video_index_rejects_empty_malformed_headers_and_record_ranges() {
        for length in 0..VIDEO_INDEX_MAGIC.len() + VIDEO_INDEX_RECORD_BYTES {
            assert!(reader(&vec![0; length], length as u64).is_err());
        }
        let entries = [VideoIndexEntry {
            index: 0,
            timestamp_ms: 0,
            duration_ms: 1,
        }];
        let mut wrong_header = bytes(&entries);
        wrong_header[0] = b'X';
        assert!(reader(&wrong_header, wrong_header.len() as u64).is_err());
        let mut partial = bytes(&entries);
        partial.push(0);
        assert!(reader(&partial, partial.len() as u64).is_err());
    }

    #[test]
    fn video_index_validates_each_record_and_detects_truncation() {
        let first = VideoIndexEntry {
            index: 0,
            timestamp_ms: 166,
            duration_ms: 100,
        };
        for second in [
            VideoIndexEntry {
                index: 2,
                timestamp_ms: 266,
                duration_ms: 100,
            },
            VideoIndexEntry {
                index: 1,
                timestamp_ms: 166,
                duration_ms: 100,
            },
            VideoIndexEntry {
                index: 1,
                timestamp_ms: 165,
                duration_ms: 100,
            },
            VideoIndexEntry {
                index: 1,
                timestamp_ms: 266,
                duration_ms: 0,
            },
        ] {
            let bytes = bytes(&[first, second]);
            let mut reader = reader(&bytes, bytes.len() as u64).unwrap();
            assert_eq!(reader.next_frame().unwrap(), Some(first));
            assert!(reader.next_frame().is_err());
        }
        let bytes = bytes(&[first]);
        let mut reader = reader(&bytes, (bytes.len() + VIDEO_INDEX_RECORD_BYTES) as u64).unwrap();
        assert_eq!(reader.next_frame().unwrap(), Some(first));
        assert!(reader.next_frame().is_err());
    }

    #[test]
    fn video_index_memory_is_bounded_independent_of_record_count() {
        let path = path();
        let mut file = File::create(&path).unwrap();
        file.write_all(VIDEO_INDEX_MAGIC).unwrap();
        let count = 10_000u64;
        let length = VIDEO_INDEX_MAGIC.len() as u64 + count * VIDEO_INDEX_RECORD_BYTES as u64;
        file.set_len(length).unwrap();
        drop(file);
        let reader = VideoIndexReader::from_range(File::open(&path).unwrap(), 0, length).unwrap();
        assert_eq!(reader.remaining(), count);
        assert_eq!(reader.input.capacity(), READ_BUFFER_BYTES);
        drop(reader);
        std::fs::remove_file(path).unwrap();
    }
}

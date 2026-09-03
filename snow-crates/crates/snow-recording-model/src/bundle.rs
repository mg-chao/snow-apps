use std::fs::{File, OpenOptions};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::Path;

use serde::{Deserialize, Serialize};

use crate::artifact::SessionManifest;
use crate::error::{RecordingModelError, Result};

const BUNDLE_FOOTER_MAGIC: &[u8; 16] = b"SNOWREC_BUNDLE\0\0";
const BUNDLE_COPY_BUFFER_BYTES: usize = 256 * 1024;

/// Kind of an auxiliary asset stored after the video payload of a bundle.
///
/// Every asset is a raw byte range described by its `BundleAssetRecord`; the
/// bundle itself never wraps assets in an additional container.
#[derive(Clone, Copy, Debug, Serialize, Deserialize, PartialEq, Eq, Hash)]
pub enum BundleAssetKind {
    /// Frame index for the video payload.
    VideoIndex,
    /// Raw interleaved PCM samples of one audio track.  The encoding, channel
    /// count and sample rate come exclusively from the matching
    /// `AudioTrackManifest` (looked up by `asset_id`); the asset carries no
    /// header, so `len / AudioTrackManifest::frame_bytes()` is the frame count.
    AudioTrack,
    /// Serialized mouse/cursor event store.
    MouseStore,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct BundleAssetRecord {
    pub kind: BundleAssetKind,
    pub asset_id: Option<String>,
    pub offset: u64,
    pub len: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct RecordingBundleFooter {
    pub manifest: SessionManifest,
    pub video_payload_len: u64,
    pub assets: Vec<BundleAssetRecord>,
}

#[derive(Clone, Copy, Debug)]
pub struct RecordingBundleAsset<'a> {
    pub kind: BundleAssetKind,
    pub asset_id: Option<&'a str>,
    pub path: &'a Path,
}

impl RecordingBundleFooter {
    pub fn asset(
        &self,
        kind: BundleAssetKind,
        asset_id: Option<&str>,
    ) -> Option<&BundleAssetRecord> {
        self.assets
            .iter()
            .find(|asset| asset.kind == kind && asset.asset_id.as_deref() == asset_id)
    }
}

pub fn write_recording_bundle(
    bundle_path: &Path,
    manifest: &SessionManifest,
    assets: &[RecordingBundleAsset<'_>],
) -> Result<()> {
    let mut bundle = OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(false)
        .open(bundle_path)?;
    let video_payload_len = bundle.metadata()?.len();
    bundle.seek(SeekFrom::Start(video_payload_len))?;

    let mut bundle_writer = BufWriter::with_capacity(BUNDLE_COPY_BUFFER_BYTES, bundle);
    let mut records = Vec::with_capacity(assets.len());
    let mut copy_buffer = vec![0u8; BUNDLE_COPY_BUFFER_BYTES];
    let mut next_offset = video_payload_len;

    for asset in assets {
        let asset_len = copy_asset_into_bundle(&mut bundle_writer, asset.path, &mut copy_buffer)?;
        records.push(BundleAssetRecord {
            kind: asset.kind,
            asset_id: asset.asset_id.map(ToOwned::to_owned),
            offset: next_offset,
            len: asset_len,
        });
        next_offset = next_offset.saturating_add(asset_len);
    }

    let footer = RecordingBundleFooter {
        manifest: manifest.clone(),
        video_payload_len,
        assets: records,
    };
    let footer_bytes = bincode::serialize(&footer).map_err(|err| {
        RecordingModelError::Decode(format!("failed to encode bundle footer: {err}"))
    })?;
    let footer_len = u64::try_from(footer_bytes.len())
        .map_err(|_| RecordingModelError::Decode("bundle footer exceeds u64 length".to_string()))?;

    bundle_writer.write_all(&footer_bytes)?;
    bundle_writer.write_all(&footer_len.to_le_bytes())?;
    bundle_writer.write_all(BUNDLE_FOOTER_MAGIC)?;
    bundle_writer.flush()?;
    Ok(())
}

pub fn read_recording_bundle_footer(bundle_path: &Path) -> Result<RecordingBundleFooter> {
    let mut bundle = File::open(bundle_path)?;
    let bundle_len = bundle.metadata()?.len();
    let trailer_len = bundle_footer_trailer_len();
    if bundle_len < trailer_len {
        return Err(RecordingModelError::Decode(format!(
            "bundle footer trailer missing from {}",
            bundle_path.display()
        )));
    }

    bundle.seek(SeekFrom::End(-(BUNDLE_FOOTER_MAGIC.len() as i64)))?;
    let mut magic = [0u8; BUNDLE_FOOTER_MAGIC.len()];
    bundle.read_exact(&mut magic)?;
    if &magic != BUNDLE_FOOTER_MAGIC {
        return Err(RecordingModelError::Decode(format!(
            "invalid bundle footer magic in {}",
            bundle_path.display()
        )));
    }

    bundle.seek(SeekFrom::End(-((BUNDLE_FOOTER_MAGIC.len() + 8) as i64)))?;
    let mut footer_len_bytes = [0u8; 8];
    bundle.read_exact(&mut footer_len_bytes)?;
    let footer_len = u64::from_le_bytes(footer_len_bytes);
    if footer_len > bundle_len.saturating_sub(trailer_len) {
        return Err(RecordingModelError::Decode(format!(
            "corrupt bundle footer length in {}",
            bundle_path.display()
        )));
    }

    let footer_offset = bundle_len - trailer_len - footer_len;
    bundle.seek(SeekFrom::Start(footer_offset))?;
    let mut footer_bytes = vec![0u8; footer_len as usize];
    bundle.read_exact(&mut footer_bytes)?;
    let footer: RecordingBundleFooter = bincode::deserialize(&footer_bytes).map_err(|err| {
        RecordingModelError::Decode(format!("failed to decode bundle footer: {err}"))
    })?;
    validate_bundle_footer(bundle_path, bundle_len, &footer)?;
    Ok(footer)
}

pub fn read_recording_bundle_asset(
    bundle_path: &Path,
    kind: BundleAssetKind,
    asset_id: Option<&str>,
) -> Result<Option<Vec<u8>>> {
    let footer = read_recording_bundle_footer(bundle_path)?;
    let Some(asset) = footer.asset(kind, asset_id) else {
        return Ok(None);
    };
    let mut bundle = File::open(bundle_path)?;
    bundle.seek(SeekFrom::Start(asset.offset))?;
    let mut bytes = vec![0u8; asset.len as usize];
    bundle.read_exact(&mut bytes)?;
    Ok(Some(bytes))
}

fn copy_asset_into_bundle(
    bundle_writer: &mut BufWriter<File>,
    asset_path: &Path,
    copy_buffer: &mut [u8],
) -> Result<u64> {
    let asset_file = File::open(asset_path)?;
    let mut asset_reader = BufReader::with_capacity(BUNDLE_COPY_BUFFER_BYTES, asset_file);
    let mut written = 0u64;

    loop {
        let read = asset_reader.read(copy_buffer)?;
        if read == 0 {
            break;
        }
        bundle_writer.write_all(&copy_buffer[..read])?;
        written = written.saturating_add(read as u64);
    }

    Ok(written)
}

fn validate_bundle_footer(
    bundle_path: &Path,
    bundle_len: u64,
    footer: &RecordingBundleFooter,
) -> Result<()> {
    let trailer_len = bundle_footer_trailer_len();
    if footer.video_payload_len > bundle_len.saturating_sub(trailer_len) {
        return Err(RecordingModelError::Decode(format!(
            "bundle video payload exceeds file length in {}",
            bundle_path.display()
        )));
    }

    let mut expected_offset = footer.video_payload_len;
    for asset in &footer.assets {
        if asset.offset != expected_offset {
            return Err(RecordingModelError::Decode(format!(
                "bundle asset layout is non-contiguous in {}",
                bundle_path.display()
            )));
        }
        expected_offset = expected_offset.saturating_add(asset.len);
    }

    if expected_offset > bundle_len.saturating_sub(trailer_len) {
        return Err(RecordingModelError::Decode(format!(
            "bundle asset payload exceeds footer boundary in {}",
            bundle_path.display()
        )));
    }

    Ok(())
}

#[inline]
const fn bundle_footer_trailer_len() -> u64 {
    8 + BUNDLE_FOOTER_MAGIC.len() as u64
}

#[cfg(test)]
mod tests {
    use std::fs;

    use uuid::Uuid;

    use super::*;
    use crate::{
        AudioSampleFormat, AudioTrackManifest, AudioTrackRole, IntermediateRecordingProfile,
        VideoEncodeConfig,
    };

    #[test]
    fn bundle_footer_roundtrip_and_asset_reads() {
        let root = std::env::temp_dir().join(format!(
            "snow-recording-model-bundle-{}",
            Uuid::new_v4().simple()
        ));
        fs::create_dir_all(&root).unwrap();

        let bundle_path = root.join("session.snowrec");
        let video_bytes = b"matroska-video-payload";
        fs::write(&bundle_path, video_bytes).unwrap();

        let video_index_path = root.join("video_index.bin");
        let mouse_path = root.join("mouse.bin");
        fs::write(&video_index_path, b"video-index").unwrap();
        fs::write(&mouse_path, b"mouse-store").unwrap();

        let manifest = SessionManifest {
            session_id: "session".to_string(),
            output_dir: root.clone(),
            keep_temp_files: false,
            fps: 30,
            intermediate_profile: IntermediateRecordingProfile::EditFast,
            recording_video: VideoEncodeConfig::default(),
            width: 1920,
            height: 1080,
            capture_origin_x: 0,
            capture_origin_y: 0,
            audio_tracks: vec![AudioTrackManifest {
                track_id: "system".to_string(),
                role: AudioTrackRole::SystemOutput,
                asset_id: "audio/system.pcm".to_string(),
                sample_rate_hz: 48_000,
                channels: 2,
                sample_format: AudioSampleFormat::PcmS16Le,
                duration_frames: 4_800,
                recorded: true,
            }],
            pause_intervals: Vec::new(),
        };

        write_recording_bundle(
            &bundle_path,
            &manifest,
            &[
                RecordingBundleAsset {
                    kind: BundleAssetKind::VideoIndex,
                    asset_id: None,
                    path: &video_index_path,
                },
                RecordingBundleAsset {
                    kind: BundleAssetKind::MouseStore,
                    asset_id: None,
                    path: &mouse_path,
                },
            ],
        )
        .unwrap();

        let footer = read_recording_bundle_footer(&bundle_path).unwrap();
        assert_eq!(footer.video_payload_len, video_bytes.len() as u64);
        assert_eq!(footer.manifest.session_id, "session");
        assert_eq!(footer.assets.len(), 2);
        assert_eq!(
            read_recording_bundle_asset(&bundle_path, BundleAssetKind::VideoIndex, None)
                .unwrap()
                .unwrap(),
            b"video-index"
        );
        assert_eq!(
            read_recording_bundle_asset(&bundle_path, BundleAssetKind::MouseStore, None)
                .unwrap()
                .unwrap(),
            b"mouse-store"
        );
    }
}

use std::error::Error;
use std::fs;
use std::path::{Path, PathBuf};

use snow_recording_model::{
    BundleAssetKind, RecordingBundleAsset, SessionManifest, write_recording_bundle,
};

fn main() -> Result<(), Box<dyn Error>> {
    let mut args = std::env::args().skip(1);
    let source = args
        .next()
        .ok_or("usage: repack_bundle <manifest.json|temp-dir> [output.snowrec]")?;
    let source_path = PathBuf::from(source);
    let manifest_path = manifest_path_for(&source_path);
    let manifest: SessionManifest = serde_json::from_slice(&fs::read(&manifest_path)?)?;

    let output_path = args.next().map(PathBuf::from).unwrap_or_else(|| {
        manifest
            .output_dir
            .join(format!("{}-rebundled.snowrec", manifest.session_id))
    });

    if let Some(parent) = output_path.parent()
        && !parent.as_os_str().is_empty()
    {
        fs::create_dir_all(parent)?;
    }

    if output_path.exists() {
        fs::remove_file(&output_path)?;
    }
    fs::copy(&source_path, &output_path)?;
    let video_index_path = source_path.join("video_index.bin");
    let mouse_path = source_path.join("mouse.bin");
    let audio_entries: Vec<(String, PathBuf)> = manifest
        .audio_tracks
        .iter()
        .filter(|track| track.recorded)
        .map(|track| {
            (
                track.asset_id.clone(),
                source_path.join(format!("audio-{}.pcm", track.track_id)),
            )
        })
        .filter(|(_, path)| path.is_file())
        .collect();
    let mut assets = vec![
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
    ];

    for (asset_id, path) in &audio_entries {
        assets.push(RecordingBundleAsset {
            kind: BundleAssetKind::AudioTrack,
            asset_id: Some(asset_id.as_str()),
            path,
        });
    }

    write_recording_bundle(&output_path, &manifest, &assets)?;
    println!("Created bundle: {}", output_path.display());
    Ok(())
}

fn manifest_path_for(source: &Path) -> PathBuf {
    if source
        .file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| name.eq_ignore_ascii_case("manifest.json"))
    {
        return source.to_path_buf();
    }
    source.join("manifest.json")
}

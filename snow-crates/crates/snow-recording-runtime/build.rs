use std::env;
use std::ffi::OsStr;
use std::fs;
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::thread;
use std::time::Duration;

fn main() {
    println!("cargo:rerun-if-env-changed=FFMPEG_DIR");
    println!("cargo:rerun-if-env-changed=VCPKGRS_DYNAMIC");

    if env::var("VCPKGRS_DYNAMIC").is_ok_and(|value| value == "0") {
        return;
    }

    let Some(ffmpeg_dir_raw) = env::var_os("FFMPEG_DIR").map(PathBuf::from) else {
        println!("cargo:warning=FFMPEG_DIR is not set; skipping FFmpeg DLL deployment");
        return;
    };
    let ffmpeg_dir = resolve_ffmpeg_dir(&ffmpeg_dir_raw);

    let profile = env::var("PROFILE").unwrap_or_else(|_| "debug".to_string());
    let Some(source_dir) = select_ffmpeg_bin_dir(&ffmpeg_dir, &profile) else {
        println!(
            "cargo:warning=No FFmpeg DLL folder found under {}",
            ffmpeg_dir.display()
        );
        return;
    };

    let Some(profile_dir) = resolve_profile_dir() else {
        println!("cargo:warning=Unable to resolve Cargo profile output directory");
        return;
    };

    for destination in [profile_dir.clone(), profile_dir.join("examples")] {
        if let Err(err) = copy_dlls(&source_dir, &destination) {
            println!(
                "cargo:warning=Failed to copy FFmpeg DLLs to {}: {err}",
                destination.display()
            );
        }
    }
}

fn resolve_ffmpeg_dir(path: &Path) -> PathBuf {
    if path.is_absolute() {
        return path.to_path_buf();
    }

    let Some(manifest_dir) = env::var_os("CARGO_MANIFEST_DIR").map(PathBuf::from) else {
        return path.to_path_buf();
    };
    let workspace_root = manifest_dir
        .ancestors()
        .nth(2)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| manifest_dir.clone());

    let workspace_relative = workspace_root.join(path);
    if workspace_relative.exists() {
        return workspace_relative;
    }

    let manifest_relative = manifest_dir.join(path);
    if manifest_relative.exists() {
        return manifest_relative;
    }

    path.to_path_buf()
}

fn select_ffmpeg_bin_dir(ffmpeg_dir: &Path, profile: &str) -> Option<PathBuf> {
    let mut candidates = Vec::with_capacity(2);
    if profile == "debug" {
        candidates.push(ffmpeg_dir.join("debug").join("bin"));
    }
    candidates.push(ffmpeg_dir.join("bin"));
    candidates.into_iter().find(|path| path.is_dir())
}

fn resolve_profile_dir() -> Option<PathBuf> {
    let out_dir = env::var_os("OUT_DIR").map(PathBuf::from)?;
    let build_dir = out_dir
        .ancestors()
        .find(|path| path.file_name() == Some(OsStr::new("build")))?;
    build_dir.parent().map(Path::to_path_buf)
}

fn copy_dlls(source_dir: &Path, destination_dir: &Path) -> io::Result<()> {
    fs::create_dir_all(destination_dir)?;

    for entry in fs::read_dir(source_dir)? {
        let entry = entry?;
        let source = entry.path();
        if !is_dll_file(&source) {
            continue;
        }

        println!("cargo:rerun-if-changed={}", source.display());

        let destination = destination_dir.join(entry.file_name());
        copy_dll_if_changed(&source, &destination)?;
    }

    Ok(())
}

fn copy_dll_if_changed(source: &Path, destination: &Path) -> io::Result<()> {
    if files_are_identical_with_retry(source, destination)? {
        return Ok(());
    }

    match fs::copy(source, destination) {
        Ok(_) => Ok(()),
        Err(copy_error) => {
            // Another build script may deploy the same DLL concurrently.
            // If it finishes after our initial comparison, no work remains.
            if wait_for_identical_copy(source, destination).unwrap_or(false) {
                Ok(())
            } else {
                Err(copy_error)
            }
        }
    }
}

fn files_are_identical_with_retry(source: &Path, destination: &Path) -> io::Result<bool> {
    for attempt in 0..=20 {
        match files_are_identical(source, destination) {
            Ok(identical) => return Ok(identical),
            Err(_) if attempt < 20 => thread::sleep(Duration::from_millis(25)),
            Err(error) => return Err(error),
        }
    }

    unreachable!()
}

fn wait_for_identical_copy(source: &Path, destination: &Path) -> io::Result<bool> {
    for attempt in 0..=20 {
        match files_are_identical(source, destination) {
            Ok(true) => return Ok(true),
            Ok(false) | Err(_) if attempt < 20 => thread::sleep(Duration::from_millis(25)),
            Ok(false) => return Ok(false),
            Err(error) => return Err(error),
        }
    }

    unreachable!()
}

fn files_are_identical(source: &Path, destination: &Path) -> io::Result<bool> {
    let destination_metadata = match fs::metadata(destination) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error),
    };
    let source_metadata = fs::metadata(source)?;
    if source_metadata.len() != destination_metadata.len() {
        return Ok(false);
    }

    let mut source = fs::File::open(source)?;
    let mut destination = fs::File::open(destination)?;
    let mut source_buffer = [0_u8; 64 * 1024];
    let mut destination_buffer = [0_u8; 64 * 1024];
    let mut remaining = source_metadata.len();

    while remaining > 0 {
        let length = remaining.min(source_buffer.len() as u64) as usize;
        source.read_exact(&mut source_buffer[..length])?;
        destination.read_exact(&mut destination_buffer[..length])?;
        if source_buffer[..length] != destination_buffer[..length] {
            return Ok(false);
        }
        remaining -= length as u64;
    }

    Ok(true)
}

fn is_dll_file(path: &Path) -> bool {
    path.is_file()
        && path
            .extension()
            .and_then(OsStr::to_str)
            .is_some_and(|ext| ext.eq_ignore_ascii_case("dll"))
}

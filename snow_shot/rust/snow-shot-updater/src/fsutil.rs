use crate::error::{Result, UpdateError, io_error, require};
use sha2::{Digest, Sha256};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::Path;

pub fn read_limited(path: &Path, limit: u64) -> Result<Vec<u8>> {
    let mut file = File::open(path).map_err(|error| {
        io_error(
            "update_file_unreadable",
            "Update file could not be read or exceeds its size limit",
            error,
        )
    })?;
    let size = file
        .metadata()
        .map_err(|error| {
            io_error(
                "update_file_unreadable",
                "Update file could not be read or exceeds its size limit",
                error,
            )
        })?
        .len();
    require(
        size <= limit,
        "update_file_unreadable",
        "Update file could not be read or exceeds its size limit",
    )?;
    let mut bytes = Vec::with_capacity(size as usize);
    file.read_to_end(&mut bytes).map_err(|error| {
        io_error(
            "update_file_read_failed",
            "Could not read update file",
            error,
        )
    })?;
    Ok(bytes)
}

pub fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path.parent().ok_or_else(|| {
        UpdateError::new("update_state_save_failed", "Could not save update state")
    })?;
    fs::create_dir_all(parent).map_err(|error| {
        io_error(
            "update_state_save_failed",
            "Could not save update state",
            error,
        )
    })?;
    let mut temporary = tempfile::NamedTempFile::new_in(parent).map_err(|error| {
        io_error(
            "update_state_save_failed",
            "Could not save update state",
            error,
        )
    })?;
    temporary
        .write_all(bytes)
        .and_then(|_| temporary.flush())
        .and_then(|_| temporary.as_file().sync_all())
        .map_err(|error| {
            io_error(
                "update_state_persist_failed",
                "Could not persist update state",
                error,
            )
        })?;
    let temporary_path = temporary.into_temp_path();
    crate::platform::replace_file(&temporary_path, path)?;
    Ok(())
}

pub fn sha256_file(path: &Path) -> Result<String> {
    let mut file = File::open(path).map_err(|error| {
        io_error(
            "update_payload_read_failed",
            "Could not read update payload",
            error,
        )
    })?;
    let mut hash = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let read = file.read(&mut buffer).map_err(|error| {
            io_error(
                "update_payload_hash_failed",
                "Could not hash update payload",
                error,
            )
        })?;
        if read == 0 {
            break;
        }
        hash.update(&buffer[..read]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

pub fn verify_file(path: &Path, size: u64, hash: &str) -> Result<()> {
    let actual_size = fs::metadata(path)
        .map(|metadata| metadata.len())
        .unwrap_or(u64::MAX);
    require(
        actual_size == size && sha256_file(path)? == hash,
        "update_payload_mismatch",
        "Update payload size or checksum does not match the signed release",
    )
}

pub fn copy_and_persist(source: &Path, destination: &Path) -> Result<()> {
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|error| {
            io_error(
                "update_destination_create_failed",
                "Could not create update destination",
                error,
            )
        })?;
    }
    fs::copy(source, destination).map_err(|error| {
        io_error(
            "update_file_stage_failed",
            "Could not stage an update file",
            error,
        )
    })?;
    OpenOptions::new()
        .write(true)
        .open(destination)
        .and_then(|file| file.sync_all())
        .map_err(|error| {
            io_error(
                "update_state_persist_failed",
                "Could not persist update state",
                error,
            )
        })
}

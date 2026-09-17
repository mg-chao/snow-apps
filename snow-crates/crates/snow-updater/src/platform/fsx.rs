//! File-system primitives with the helper's durability rules: long paths,
//! link rejection, atomic replacement, flushed writes, and the transaction
//! lock used to serialize apply/recover/uninstall.

use super::wide;
use crate::errors::{Error, Result, require};
use crate::paths::absolute_path;
use std::fs;
use std::io::{Read, Write};
use std::os::windows::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use windows::Win32::Foundation::CloseHandle;
use windows::Win32::Storage::FileSystem::{
    CreateFileW, DeleteFileW, FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL,
    FILE_ATTRIBUTE_REPARSE_POINT, FILE_BEGIN, FILE_GENERIC_WRITE, FILE_SHARE_DELETE,
    FILE_SHARE_READ, FILE_SHARE_WRITE, FlushFileBuffers, GetDiskFreeSpaceExW, GetFileAttributesW,
    INVALID_FILE_ATTRIBUTES, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
    OPEN_ALWAYS, OPEN_EXISTING, SetEndOfFile, SetFilePointerEx, WriteFile,
};
use windows::core::PCWSTR;

/// NUL-terminated `\\?\`-prefixed native form for pointer-based Win32 calls.
pub fn long_wide(path: &Path) -> Vec<u16> {
    wide(long_form(path).as_os_str())
}

/// `\\?\`-prefixed path usable by `std::fs`, so operations work beyond legacy
/// MAX_PATH.
pub fn long_path(path: &Path) -> PathBuf {
    let form = long_form(path);
    let mut units: Vec<u16> = form.as_os_str().encode_wide().collect();
    let end = units
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(units.len());
    units.truncate(end);
    PathBuf::from(std::ffi::OsString::from_wide(&units))
}

fn long_form(path: &Path) -> PathBuf {
    let absolute = absolute_path(path);
    let text = absolute.to_string_lossy().replace('/', "\\");
    if let Some(rest) = text.strip_prefix("\\\\") {
        PathBuf::from(format!("\\\\?\\UNC\\{rest}"))
    } else {
        PathBuf::from(format!("\\\\?\\{text}"))
    }
}

pub fn file_exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

pub fn is_file(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|meta| meta.is_file())
}

pub fn is_dir(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|meta| meta.is_dir())
}

pub fn file_size(path: &Path) -> u64 {
    fs::symlink_metadata(path)
        .map(|meta| meta.len())
        .unwrap_or(0)
}

pub fn create_dir_all(path: &Path) -> Result<()> {
    fs::create_dir_all(long_path(path))
        .or_else(|_| fs::create_dir_all(path))
        .map_err(|_| Error::fixed("Could not create update destination"))
}

/// Reads a file while enforcing the metadata size cap.
pub fn read_limited(path: &Path, limit: u64) -> Result<Vec<u8>> {
    let file = fs::File::open(path)
        .map_err(|_| Error::fixed("Update file could not be read or exceeds its size limit"))?;
    let size = file
        .metadata()
        .map_err(|_| Error::fixed("Update file could not be read or exceeds its size limit"))?
        .len();
    require(
        size <= limit,
        "Update file could not be read or exceeds its size limit",
    )?;
    let mut bytes = Vec::with_capacity(size as usize);
    let mut file = file;
    file.read_to_end(&mut bytes)
        .map_err(|_| Error::fixed("Could not read update file"))?;
    Ok(bytes)
}

/// QSaveFile-equivalent atomic write: same-directory temporary, flushed,
/// then replaced through the write-through move.
pub fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let directory = path.parent().unwrap_or(Path::new("."));
    require(
        fs::create_dir_all(long_path(directory))
            .or_else(|_| fs::create_dir_all(directory))
            .is_ok(),
        "Could not save update state",
    )?;
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_default();
    let temporary = directory.join(format!(".{name}.{}", uuid::Uuid::new_v4().simple()));
    let mut file = fs::File::options()
        .write(true)
        .create_new(true)
        .open(&temporary)
        .map_err(|_| Error::fixed("Could not save update state"))?;
    file.write_all(bytes)
        .map_err(|_| Error::fixed("Could not save update state"))?;
    drop(file);
    persist_file(&temporary)?;
    let moved = unsafe {
        MoveFileExW(
            PCWSTR(long_wide(&temporary).as_ptr()),
            PCWSTR(long_wide(path).as_ptr()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if !moved.is_ok() {
        let _ = fs::remove_file(&temporary);
        return Err(Error::fixed("Could not commit update state"));
    }
    Ok(())
}

/// Flushes an existing file's buffers to durable storage.
pub fn persist_file(path: &Path) -> Result<()> {
    unsafe {
        let Ok(handle) = CreateFileW(
            PCWSTR(long_wide(path).as_ptr()),
            FILE_GENERIC_WRITE.0,
            FILE_SHARE_READ,
            None,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            None,
        ) else {
            return Err(Error::fixed("Could not persist update state"));
        };
        let flushed = FlushFileBuffers(handle).is_ok();
        let _ = CloseHandle(handle);
        require(flushed, "Could not persist update state")
    }
}

/// Rejects symbolic links and any reparse points along the whole chain.
pub fn no_links(path: &Path) -> Result<()> {
    let mut current = absolute_path(path);
    loop {
        let attributes = unsafe { GetFileAttributesW(PCWSTR(long_wide(&current).as_ptr())) };
        if attributes != INVALID_FILE_ATTRIBUTES {
            require(
                attributes & FILE_ATTRIBUTE_REPARSE_POINT.0 == 0,
                "Update paths must not contain reparse points",
            )?;
        }
        let parent = match current.parent() {
            Some(parent) if parent != current => parent.to_path_buf(),
            _ => break,
        };
        current = parent;
    }
    Ok(())
}

/// Copy plus flushed, atomic, write-through replacement of the destination.
pub fn atomic_copy(source: &Path, destination: &Path) -> Result<()> {
    no_links(destination)?;
    if let Some(parent) = destination.parent() {
        create_dir_all(parent).map_err(|_| Error::fixed("Could not create update destination"))?;
    }
    // The temporary name lives beside the destination, matching the
    // historical layout, and is renamed over it after flushing.
    let temporary = destination.parent().unwrap_or(Path::new(".")).join(format!(
        "{}.snow-update-{}",
        destination
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
        uuid::Uuid::new_v4().simple()
    ));
    let _ = fs::remove_file(&temporary);
    fs::copy(long_path(source), &temporary)
        .map_err(|_| Error::fixed("Could not stage an update file"))?;
    persist_file(&temporary)?;
    let moved = unsafe {
        MoveFileExW(
            PCWSTR(long_wide(&temporary).as_ptr()),
            PCWSTR(long_wide(destination).as_ptr()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    };
    if !moved.is_ok() {
        let _ = fs::remove_file(&temporary);
        return Err(Error::fixed(
            "Could not replace an update file; close applications using it",
        ));
    }
    Ok(())
}

/// Removes a work tree after verifying every entry is free of links.
pub fn clear_work_tree(path: &Path) -> Result<()> {
    no_links(path)?;
    if file_exists(path) {
        let long = long_path(path);
        collect_and_check_links(&long)?;
        fs::remove_dir_all(&long)
            .or_else(|_| fs::remove_dir_all(path))
            .map_err(|_| Error::fixed("Could not clear update staging directory"))?;
    }
    Ok(())
}

fn collect_and_check_links(directory: &Path) -> Result<()> {
    let entries = fs::read_dir(directory)
        .map_err(|_| Error::fixed("Could not clear update staging directory"))?;
    for entry in entries.flatten() {
        let path = entry.path();
        no_links(&path)?;
        if path.symlink_metadata().is_ok_and(|meta| meta.is_dir()) && !path.is_symlink() {
            collect_and_check_links(&path)?;
        }
    }
    Ok(())
}

pub fn free_space(path: &Path) -> Option<u64> {
    let mut available = 0u64;
    let root = long_wide(&absolute_path(path));
    let ok = unsafe {
        GetDiskFreeSpaceExW(PCWSTR(root.as_ptr()), Some(&mut available), None, None).is_ok()
    };
    ok.then_some(available)
}

pub fn last_modified_before(path: &Path, seconds_ago: u64) -> bool {
    let Ok(meta) = fs::symlink_metadata(path) else {
        return false;
    };
    let Ok(modified) = meta.modified() else {
        return false;
    };
    let Ok(age) = modified.duration_since(UNIX_EPOCH) else {
        return false;
    };
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .is_ok_and(|now| now.as_secs().saturating_sub(age.as_secs()) > seconds_ago)
}

/// Deletes a file through the long-path form; best effort.
pub fn remove_file(path: &Path) -> bool {
    if unsafe { DeleteFileW(PCWSTR(long_wide(path).as_ptr())) }.is_ok() {
        return true;
    }
    fs::remove_file(path).is_ok()
}

/// Cross-process transaction lock with QLockFile semantics: an exclusively
/// shared open held by the owner; dead-owner lock files are reclaimed.
pub struct TransactionLock {
    handle: Option<windows::Win32::Foundation::HANDLE>,
    path: PathBuf,
}

impl TransactionLock {
    pub fn try_lock(path: &Path) -> Option<TransactionLock> {
        for _ in 0..2 {
            unsafe {
                let handle = CreateFileW(
                    PCWSTR(long_wide(path).as_ptr()),
                    FILE_GENERIC_WRITE.0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    None,
                    OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    None,
                );
                let handle = match handle {
                    Ok(handle) => handle,
                    Err(_) => {
                        if read_lock_pid(path).is_some_and(|pid| !process_alive(pid)) {
                            let _ = DeleteFileW(PCWSTR(long_wide(path).as_ptr()));
                            continue;
                        }
                        return None;
                    }
                };
                let _ = SetFilePointerEx(handle, 0, None, FILE_BEGIN);
                let text = format!("{}\n", std::process::id());
                let mut written = 0u32;
                let _ = WriteFile(handle, Some(text.as_bytes()), Some(&mut written), None);
                let _ = SetEndOfFile(handle);
                let _ = FlushFileBuffers(handle);
                return Some(TransactionLock {
                    handle: Some(handle),
                    path: path.to_path_buf(),
                });
            }
        }
        None
    }

    pub fn unlock(&mut self) {
        if let Some(handle) = self.handle.take() {
            unsafe {
                let _ = CloseHandle(handle);
                let _ = DeleteFileW(PCWSTR(long_wide(&self.path).as_ptr()));
            }
        }
    }
}

impl Drop for TransactionLock {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            unsafe {
                let _ = CloseHandle(handle);
            }
        }
    }
}

fn read_lock_pid(path: &Path) -> Option<u32> {
    let bytes = fs::read(path).ok()?;
    let text = String::from_utf8_lossy(&bytes);
    text.lines().next()?.trim().parse().ok()
}

fn process_alive(pid: u32) -> bool {
    use windows::Win32::System::Threading::{OpenProcess, PROCESS_SYNCHRONIZE};
    unsafe {
        let Ok(handle) = OpenProcess(PROCESS_SYNCHRONIZE, false, pid) else {
            return false;
        };
        let _ = CloseHandle(handle);
        true
    }
}

/// Writability probe with a unique file that is removed immediately,
/// mirroring the historical QTemporaryFile check.
pub fn writable(root: &Path) -> bool {
    for _ in 0..64 {
        let path = root.join(format!(
            ".snow-shot-permission-{}",
            uuid::Uuid::new_v4().simple()
        ));
        match fs::File::options().write(true).create_new(true).open(&path) {
            Ok(file) => {
                drop(file);
                let _ = fs::remove_file(&path);
                return true;
            }
            Err(err) if err.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(_) => return false,
        }
    }
    false
}

/// Directory attributes helper for the coordinator prune pass.
pub fn attributes_without_reparse(path: &Path) -> bool {
    let attributes = unsafe { GetFileAttributesW(PCWSTR(long_wide(path).as_ptr())) };
    attributes != INVALID_FILE_ATTRIBUTES
        && attributes & FILE_ATTRIBUTE_REPARSE_POINT.0 == 0
        && attributes & FILE_ATTRIBUTE_DIRECTORY.0 != 0
}

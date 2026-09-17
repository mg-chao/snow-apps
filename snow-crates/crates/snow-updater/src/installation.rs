//! Installation identity: root validation, the installation record, the
//! pending-transaction marker, and work-area pruning.

use crate::contract::{METADATA_LIMIT, UpdateFile, parse_file_inventory};
use crate::errors::{Error, Result, require};
use crate::paths::{clean_path, path_ci_starts_with_dir, paths_ci_eq};
use crate::platform::fsx::{self, no_links};
use crate::semver::validate_version;
use serde_json::Value;
use std::path::{Path, PathBuf};

pub const RECORD_FILE: &str = "snow-shot-installation.json";
pub const WORK_DIRECTORY: &str = ".snow-shot-update";

pub fn installation_root(executable_directory: &Path) -> PathBuf {
    executable_directory
        .parent()
        .unwrap_or(Path::new("."))
        .to_path_buf()
}

pub fn path_at(root: &Path, relative: &str) -> PathBuf {
    root.join(relative)
}

pub fn work_path(root: &Path, leaf: &str) -> PathBuf {
    path_at(&path_at(root, WORK_DIRECTORY), leaf)
}

/// The portable data directory selected by `bin/__data_directory`, if any.
pub fn selected_data_root(root: &Path) -> Option<String> {
    let marker = path_at(root, "bin/__data_directory");
    let mut data = String::from("portable");
    if fsx::file_exists(&marker) {
        data = String::from_utf8_lossy(&fsx::read_limited(&marker, 32768).ok()?).into_owned();
        while data.starts_with('\u{FEFF}') {
            data.remove(0);
        }
        data = data.trim().to_owned();
    }
    if data.is_empty() {
        return None;
    }
    let joined = root.join("bin").join(&data);
    if let Ok(canonical) = std::fs::canonicalize(&joined) {
        return Some(clean_path(&crate::paths::strip_verbatim(&canonical)));
    }
    Some(clean_path(&joined.to_string_lossy()))
}

/// Test probe for the selected data root.
#[cfg(any(test, feature = "signing"))]
pub fn selected_data_root_dbg(root: &Path) -> Option<String> {
    selected_data_root(root)
}

fn validate_root(root: &Path) -> Result<()> {
    let text = clean_path(&root.to_string_lossy());
    let meta = root.symlink_metadata();
    let is_directory = meta.as_ref().is_ok_and(|meta| meta.is_dir());
    let absolute = root.is_absolute();
    let root_of_itself = {
        let drive_root = if text.len() >= 2 && text.as_bytes()[1] == b':' {
            format!("{}:/", &text[..2])
        } else if let Some(rest) = text.strip_prefix("//") {
            let mut parts = rest.splitn(3, '/');
            format!(
                "//{}/{}",
                parts.next().unwrap_or(""),
                parts.next().unwrap_or("")
            )
        } else {
            "/".to_owned()
        };
        paths_ci_eq(&text, &drive_root)
    };
    let home = std::env::var_os("USERPROFILE")
        .map(|home| clean_path(&home.to_string_lossy()))
        .filter(|home| !home.is_empty());
    let file_name_length = text.rsplit('/').next().unwrap_or_default().len();
    require(
        is_directory
            && absolute
            && !root_of_itself
            && home.as_deref().is_none_or(|home| !paths_ci_eq(&text, home))
            && file_name_length > 1,
        "Invalid Snow Shot installation root",
    )?;
    no_links(root)?;
    no_links(&work_path(root, ""))?;
    let Some(data) = selected_data_root(root) else {
        return Ok(());
    };
    let work = clean_path(&work_path(root, "").to_string_lossy());
    require(
        !paths_ci_eq(&work, &data)
            && !path_ci_starts_with_dir(&work, &data)
            && !path_ci_starts_with_dir(&data, &work),
        "An update file would overwrite the selected data directory",
    )
}

pub fn validate_installation_root(root: &Path) -> Result<()> {
    validate_root(root)
}

/// The validated installation record.
#[derive(Clone, Debug)]
pub struct InstallationRecord {
    pub variant: String,
    pub version: String,
    pub files: Vec<UpdateFile>,
}

pub fn installation_record(root: &Path) -> Result<InstallationRecord> {
    let bytes = fsx::read_limited(&path_at(root, RECORD_FILE), METADATA_LIMIT)?;
    let record: Value = serde_json::from_slice(&bytes).map_err(|_| {
        Error::fixed("This copy does not have valid Snow Shot installation metadata")
    })?;
    let variant = record
        .get("variant")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    require(
        record.get("schema").and_then(Value::as_i64) == Some(1)
            && (variant == "online" || variant == "offline" || variant == "portable"),
        "This copy does not have valid Snow Shot installation metadata",
    )?;
    let version = record
        .get("version")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    validate_version(&version)?;
    let files = parse_file_inventory(record.get("files").and_then(Value::as_array).ok_or_else(
        || Error::fixed("This copy does not have valid Snow Shot installation metadata"),
    )?)?;
    Ok(InstallationRecord {
        variant,
        version,
        files,
    })
}

/// Ensures a transaction journal path is inside the owned payload and does
/// not touch the selected data directory.
pub fn validate_target_path(root: &Path, relative: &str) -> Result<()> {
    require(
        crate::contract::safe_relative_path(relative)
            && (relative.starts_with("bin/")
                || relative.starts_with("share/snow-shot/")
                || relative == RECORD_FILE)
            && !relative.eq_ignore_ascii_case("bin/__data_directory"),
        "Refusing to change a file outside the application payload",
    )?;
    no_links(&path_at(root, relative))?;
    if let Some(data_root) = selected_data_root(root) {
        let target = clean_path(&path_at(root, relative).to_string_lossy());
        require(
            !paths_ci_eq(&target, &data_root) && !path_ci_starts_with_dir(&target, &data_root),
            "An update file would overwrite the selected data directory",
        )?;
    }
    Ok(())
}

pub fn transaction_pending(root: &Path) -> bool {
    fsx::file_exists(&work_path(root, "journal.json"))
}

/// Best-effort cleanup of generated worker/input files older than a day.
pub fn prune_update_work(root: &Path) -> Result<()> {
    validate_root(root)?;
    if transaction_pending(root) || !fsx::file_exists(&work_path(root, "")) {
        return Ok(());
    }
    let Some(_lock) = fsx::TransactionLock::try_lock(&work_path(root, "transaction.lock")) else {
        return Ok(());
    };
    let work = work_path(root, "");
    let entries = match std::fs::read_dir(&work) {
        Ok(entries) => entries,
        Err(_) => return Ok(()),
    };
    let generated = |name: &str| {
        let stem = |prefix: &str, suffix: &str| {
            name.strip_prefix(prefix)
                .and_then(|rest| rest.strip_suffix(suffix))
                .is_some_and(|stem| stem.len() == 32 && stem.chars().all(|c| c.is_ascii_hexdigit()))
        };
        stem("worker-", ".exe") || stem("input-", ".zip")
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if generated(&name) && fsx::last_modified_before(&entry.path(), 86400) {
            let _ = no_links(&entry.path());
            // Running Windows executables cannot be removed; cleanup is best
            // effort.
            let _ = fsx::remove_file(&entry.path());
        }
    }
    Ok(())
}

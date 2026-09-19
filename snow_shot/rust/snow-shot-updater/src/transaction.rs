use crate::contract::{
    UpdateFile, UpdatePackage, UpdateRelease, compare_versions, parse_file_inventory,
    safe_relative_path,
};
use crate::error::{Result, UpdateError, io_error, require};
use crate::fsutil::{copy_and_persist, read_limited, sha256_file, verify_file, write_atomic};
use crate::platform;
use fs2::FileExt;
use path_clean::PathClean;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};
use uuid::Uuid;

pub const INSTALLATION_RECORD: &str = "snow-shot-installation.json";
pub const UPDATE_WORK: &str = ".snow-shot-update";

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
pub struct InstallationRecord {
    pub schema: u64,
    pub variant: String,
    pub version: String,
    pub files: Vec<UpdateFile>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct JournalEntry {
    path: String,
    existed: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    size: Option<u64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    sha256: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct Journal {
    schema: u64,
    state: String,
    version: String,
    previous_version: String,
    files: Vec<JournalEntry>,
}

#[derive(Default)]
pub struct TransactionHooks<'a> {
    pub probe: Option<&'a dyn Fn() -> bool>,
    pub checkpoint: Option<&'a dyn Fn(&str)>,
}

fn work_path(root: &Path, leaf: impl AsRef<Path>) -> PathBuf {
    root.join(UPDATE_WORK).join(leaf)
}

pub fn installation_root(executable_directory: &Path) -> PathBuf {
    executable_directory
        .parent()
        .unwrap_or(executable_directory)
        .to_path_buf()
}

fn absolute_clean(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.clean()
    } else {
        std::env::current_dir()
            .unwrap_or_default()
            .join(path)
            .clean()
    }
}

fn no_links(path: &Path) -> Result<()> {
    let mut part = absolute_clean(path);
    loop {
        if let Ok(metadata) = fs::symlink_metadata(&part) {
            require(
                !metadata.file_type().is_symlink(),
                "update_path_symlink",
                "Update paths must not contain symbolic links",
            )?;
        }
        require(
            !platform::path_has_reparse(&part),
            "update_path_reparse",
            "Update paths must not contain reparse points",
        )?;
        if !part.pop() {
            break;
        }
    }
    Ok(())
}

fn selected_data_root(root: &Path) -> Result<Option<PathBuf>> {
    let marker = root.join("bin/__data_directory");
    let mut data = if marker.exists() {
        String::from_utf8(read_limited(&marker, 32_768)?).map_err(|_| {
            UpdateError::new(
                "data_directory_invalid",
                "An update file would overwrite the selected data directory",
            )
        })?
    } else {
        "portable".to_owned()
    };
    while data.starts_with('\u{feff}') {
        data.remove(0);
    }
    let data = data.trim();
    if data.is_empty() {
        return Ok(None);
    }
    let selected = absolute_clean(&root.join("bin").join(data));
    Ok(Some(fs::canonicalize(&selected).unwrap_or(selected)))
}

fn comparable_path(path: &Path) -> String {
    let text = absolute_clean(path)
        .to_string_lossy()
        .replace('\\', "/")
        .to_ascii_lowercase();
    if let Some(rest) = text.strip_prefix("//?/unc/") {
        format!("//{rest}")
    } else if let Some(rest) = text.strip_prefix("//?/") {
        rest.to_owned()
    } else {
        text
    }
}

fn path_starts_with_case_insensitive(path: &Path, parent: &Path) -> bool {
    let path = comparable_path(path);
    let parent = comparable_path(parent);
    path == parent
        || path
            .get(parent.len()..)
            .is_some_and(|tail| tail.starts_with('/') && path[..parent.len()] == parent)
}

fn installation_root_identity(root: &Path) -> Result<PathBuf> {
    let root = absolute_clean(root);
    let metadata = fs::metadata(&root).ok();
    let home =
        std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).map(PathBuf::from);
    require(
        root.is_absolute()
            && metadata.is_some_and(|metadata| metadata.is_dir())
            && root.parent().is_some()
            && root
                .file_name()
                .is_some_and(|name| name.to_string_lossy().encode_utf16().count() > 1)
            && home
                .as_ref()
                .is_none_or(|home| !platform::path_eq(&root, &absolute_clean(home))),
        "invalid_installation_root",
        "Invalid Snow Shot installation root",
    )?;
    Ok(root)
}

fn data_collides_with_work_tree(root: &Path, data: &Path) -> bool {
    let work = work_path(root, "");
    path_starts_with_case_insensitive(&work, data) || path_starts_with_case_insensitive(data, &work)
}

fn selected_data_root_if_known(root: &Path) -> Option<PathBuf> {
    selected_data_root(root).ok().flatten()
}

fn is_payload_relative(relative: &str) -> bool {
    safe_relative_path(relative)
        && (relative.starts_with("bin/")
            || relative.starts_with("share/snow-shot/")
            || relative == INSTALLATION_RECORD)
        && !relative.eq_ignore_ascii_case("bin/__data_directory")
}

pub fn validate_root(root: &Path) -> Result<()> {
    let root = installation_root_identity(root)?;
    no_links(&root)?;
    no_links(&work_path(&root, ""))?;
    if let Some(data) = selected_data_root(&root)? {
        require(
            !data_collides_with_work_tree(&root, &data),
            "selected_data_collision",
            "An update file would overwrite the selected data directory",
        )?;
    }
    Ok(())
}

pub fn installation_record(root: &Path) -> Result<InstallationRecord> {
    let bytes = read_limited(&root.join(INSTALLATION_RECORD), 8 * 1024 * 1024)?;
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| {
        UpdateError::new(
            "invalid_installation_metadata",
            "This copy does not have valid Snow Shot installation metadata",
        )
    })?;
    let object = value.as_object().ok_or_else(|| {
        UpdateError::new(
            "invalid_installation_metadata",
            "This copy does not have valid Snow Shot installation metadata",
        )
    })?;
    let schema = object
        .get("schema")
        .and_then(Value::as_u64)
        .unwrap_or_default();
    let variant = object
        .get("variant")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let version = object
        .get("version")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    require(
        schema == 1 && matches!(variant.as_str(), "online" | "offline" | "portable"),
        "invalid_installation_metadata",
        "This copy does not have valid Snow Shot installation metadata",
    )?;
    compare_versions(&version, &version)?;
    let files = parse_file_inventory(object.get("files"))?;
    Ok(InstallationRecord {
        schema,
        variant,
        version,
        files,
    })
}

pub fn validate_target_path(root: &Path, relative: &str) -> Result<()> {
    require(
        is_payload_relative(relative),
        "target_outside_payload",
        "Refusing to change a file outside the application payload",
    )?;
    let target = root.join(relative);
    no_links(&target)?;
    if let Some(data) = selected_data_root(root)? {
        require(
            !path_starts_with_case_insensitive(&target, &data),
            "selected_data_collision",
            "An update file would overwrite the selected data directory",
        )?;
    }
    Ok(())
}

pub fn transaction_pending(root: &Path) -> bool {
    work_path(root, "journal.json").is_file()
}

fn acquire_lock(root: &Path, message: &'static str) -> Result<File> {
    fs::create_dir_all(root.join(UPDATE_WORK)).map_err(|error| {
        io_error(
            "update_lock_directory_failed",
            "Could not create update work directory",
            error,
        )
    })?;
    let lock = OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(false)
        .open(work_path(root, "transaction.lock"))
        .map_err(|error| io_error("update_lock_failed", message, error))?;
    lock.try_lock_exclusive()
        .map_err(|error| io_error("update_lock_failed", message, error))?;
    Ok(lock)
}

fn atomic_copy(source: &Path, destination: &Path) -> Result<()> {
    no_links(destination)?;
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|error| {
            io_error(
                "update_destination_create_failed",
                "Could not create update destination",
                error,
            )
        })?;
    }
    let temporary = destination.with_file_name(format!(
        "{}.snow-update-{}",
        destination
            .file_name()
            .unwrap_or_default()
            .to_string_lossy(),
        Uuid::new_v4().simple()
    ));
    copy_and_persist(source, &temporary)?;
    let result = platform::replace_file(&temporary, destination);
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn clear_work_tree(root: &Path, name: impl AsRef<Path>) -> Result<()> {
    let path = work_path(root, name);
    no_links(&path)?;
    if !path.exists() {
        return Ok(());
    }
    for entry in walkdir::WalkDir::new(&path).follow_links(false) {
        let entry = entry.map_err(|error| {
            io_error(
                "update_stage_clear_failed",
                "Could not clear update staging directory",
                error,
            )
        })?;
        no_links(entry.path())?;
    }
    if path.is_dir() {
        fs::remove_dir_all(&path)
    } else {
        fs::remove_file(&path)
    }
    .map_err(|error| {
        io_error(
            "update_stage_clear_failed",
            "Could not clear update staging directory",
            error,
        )
    })
}

fn verify_record_inventory(record: &InstallationRecord, package: &UpdatePackage) -> Result<()> {
    let mut expected: BTreeMap<&str, (u64, &str)> = package
        .files
        .iter()
        .filter(|file| file.path != INSTALLATION_RECORD)
        .map(|file| (file.path.as_str(), (file.size, file.sha256.as_str())))
        .collect();
    for file in &record.files {
        let Some((size, hash)) = expected.remove(file.path.as_str()) else {
            return Err(UpdateError::new(
                "installation_inventory_mismatch",
                "Update installation metadata does not match the signed release",
            ));
        };
        require(
            size == file.size && hash == file.sha256,
            "installation_inventory_mismatch",
            "Update installation metadata does not match the signed release",
        )?;
    }
    require(
        expected.is_empty(),
        "installation_inventory_mismatch",
        "Update installation metadata does not match the signed release",
    )
}

fn extract(archive_path: &Path, destination: &Path, package: &UpdatePackage) -> Result<()> {
    let archive_file = File::open(archive_path).map_err(|error| {
        io_error(
            "archive_open_failed",
            "Could not open update archive",
            error,
        )
    })?;
    let mut archive = zip::ZipArchive::new(archive_file).map_err(|error| {
        io_error(
            "archive_open_failed",
            "Could not open update archive",
            error,
        )
    })?;
    // Validate the complete central directory before creating a single staged file. This keeps
    // unknown, duplicate, encrypted, linked, or otherwise unsigned data from being partially
    // materialized even inside the isolated stage tree.
    let mut expected: HashMap<String, UpdateFile> = package
        .files
        .iter()
        .cloned()
        .map(|file| (file.path.clone(), file))
        .collect();
    for index in 0..archive.len() {
        let entry = archive.by_index_raw(index).map_err(|error| {
            io_error(
                "invalid_archive_entry",
                "Invalid update archive entry",
                error,
            )
        })?;
        let name = entry.name().to_owned();
        let descriptor = expected.remove(&name).ok_or_else(|| {
            UpdateError::new(
                "unsafe_archive_entry",
                "Unexpected, duplicate, or unsafe update archive entry",
            )
        })?;
        let mode = entry.unix_mode().unwrap_or(0) & 0o170000;
        require(
            safe_relative_path(&name)
                && !entry.is_dir()
                && !entry.encrypted()
                && (mode == 0 || mode == 0o100000)
                && matches!(
                    entry.compression(),
                    zip::CompressionMethod::Stored | zip::CompressionMethod::Deflated
                )
                && entry.size() == descriptor.size,
            "unsafe_archive_entry",
            "Unexpected, duplicate, or unsafe update archive entry",
        )?;
    }
    require(
        expected.is_empty(),
        "incomplete_update_archive",
        "Incomplete update archive",
    )?;

    let descriptors: HashMap<&str, &UpdateFile> = package
        .files
        .iter()
        .map(|file| (file.path.as_str(), file))
        .collect();
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|error| {
            io_error(
                "invalid_archive_entry",
                "Invalid update archive entry",
                error,
            )
        })?;
        let name = entry.name().to_owned();
        let descriptor = descriptors.get(name.as_str()).ok_or_else(|| {
            UpdateError::new(
                "unsafe_archive_entry",
                "Unexpected, duplicate, or unsafe update archive entry",
            )
        })?;
        let output = destination.join(&name);
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent).map_err(|error| {
                io_error(
                    "archive_stage_directory_failed",
                    "Could not create update staging directory",
                    error,
                )
            })?;
        }
        let mut file = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&output)
            .map_err(|error| {
                io_error(
                    "archive_extract_failed",
                    "Could not extract update archive entry",
                    error,
                )
            })?;
        let mut buffer = [0_u8; 64 * 1024];
        let mut total = 0_u64;
        loop {
            let count = entry.read(&mut buffer).map_err(|error| {
                io_error(
                    "archive_entry_corrupt",
                    "Corrupt update archive entry",
                    error,
                )
            })?;
            if count == 0 {
                break;
            }
            total = total.checked_add(count as u64).ok_or_else(|| {
                UpdateError::new(
                    "archive_entry_write_failed",
                    "Could not write update archive entry",
                )
            })?;
            require(
                total <= descriptor.size,
                "archive_entry_write_failed",
                "Could not write update archive entry",
            )?;
            file.write_all(&buffer[..count]).map_err(|error| {
                io_error(
                    "archive_entry_write_failed",
                    "Could not write update archive entry",
                    error,
                )
            })?;
        }
        file.flush().map_err(|error| {
            io_error(
                "archive_entry_write_failed",
                "Could not write update archive entry",
                error,
            )
        })?;
        drop(file);
        verify_file(&output, descriptor.size, &descriptor.sha256)?;
    }
    Ok(())
}

fn save_journal(root: &Path, journal: &Journal) -> Result<()> {
    let bytes = serde_json::to_vec(journal).map_err(|error| {
        io_error(
            "update_state_save_failed",
            "Could not save update state",
            error,
        )
    })?;
    write_atomic(&work_path(root, "journal.json"), &bytes)
}

fn restore(root: &Path) -> Result<()> {
    let journal: Journal = serde_json::from_slice(&read_limited(
        &work_path(root, "journal.json"),
        8 * 1024 * 1024,
    )?)
    .map_err(|_| {
        UpdateError::new(
            "unsupported_recovery_journal",
            "Unsupported update recovery journal",
        )
    })?;
    require(
        journal.schema == 1,
        "unsupported_recovery_journal",
        "Unsupported update recovery journal",
    )?;
    if journal.state == "committed" {
        fs::remove_file(work_path(root, "journal.json")).map_err(|error| {
            io_error(
                "committed_state_finalize_failed",
                "Could not finalize committed update state",
                error,
            )
        })?;
        return Ok(());
    }
    for entry in &journal.files {
        validate_target_path(root, &entry.path)?;
        if entry.existed {
            let backup = work_path(root, Path::new("backup").join(&entry.path));
            no_links(&backup)?;
            verify_file(
                &backup,
                entry.size.unwrap_or(u64::MAX),
                entry.sha256.as_deref().unwrap_or_default(),
            )?;
        }
    }
    for entry in &journal.files {
        let destination = root.join(&entry.path);
        if entry.existed {
            atomic_copy(
                &work_path(root, Path::new("backup").join(&entry.path)),
                &destination,
            )?;
        } else if destination.exists() {
            fs::remove_file(&destination).map_err(|error| {
                io_error(
                    "incomplete_file_remove_failed",
                    "Could not remove an incomplete update file",
                    error,
                )
            })?;
        }
    }
    platform::write_registered_version(root, &journal.previous_version)?;
    write_atomic(
        &work_path(root, "failed-version.txt"),
        journal.version.as_bytes(),
    )?;
    fs::remove_file(work_path(root, "journal.json")).map_err(|error| {
        io_error(
            "recovery_finalize_failed",
            "Could not finalize update recovery",
            error,
        )
    })
}

pub fn recover_transaction(root: &Path) -> Result<()> {
    validate_root(root)?;
    let _lock = acquire_lock(root, "Another update transaction is running")?;
    if transaction_pending(root) {
        restore(root)?;
    }
    Ok(())
}

fn run_with_timeout(command: &mut Command, timeout: Duration) -> bool {
    let Ok(mut child) = command.spawn() else {
        return false;
    };
    let deadline = Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return status.success(),
            Ok(None) if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(50));
            }
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return false;
            }
        }
    }
}

pub fn apply_transaction(
    root: &Path,
    archive: &Path,
    release: &UpdateRelease,
    hooks: TransactionHooks<'_>,
) -> Result<()> {
    validate_root(root)?;
    fs::create_dir_all(root.join(UPDATE_WORK)).map_err(|error| {
        io_error(
            "update_work_create_failed",
            "Could not create update work directory",
            error,
        )
    })?;
    let _lock = acquire_lock(root, "Another update transaction is running")?;
    if transaction_pending(root) {
        restore(root)?;
    }
    let installed = installation_record(root)?;
    require(
        compare_versions(&release.version, &installed.version)? == Ordering::Greater,
        "update_not_newer",
        "The update must be newer than the installed release",
    )?;
    let package = release.update_package(&installed.variant)?;
    verify_file(archive, package.size, &package.sha256)?;
    let needed = package.files.iter().try_fold(package.size, |total, file| {
        file.size
            .checked_mul(3)
            .and_then(|size| total.checked_add(size))
    });
    let available = fs2::available_space(root).unwrap_or_default();
    require(
        needed.is_some_and(|needed| available > needed.saturating_add(64 * 1024 * 1024)),
        "insufficient_update_space",
        "Not enough free space to stage and recover this update",
    )?;
    clear_work_tree(root, "stage")?;
    clear_work_tree(root, "backup")?;
    let stage = work_path(root, "stage");
    fs::create_dir_all(&stage).map_err(|error| {
        io_error(
            "archive_stage_directory_failed",
            "Could not create update staging directory",
            error,
        )
    })?;
    extract(archive, &stage, package)?;
    let staged_record = installation_record(&stage)?;
    verify_record_inventory(&staged_record, package)?;
    require(
        staged_record.version == release.version && staged_record.variant == installed.variant,
        "installation_inventory_mismatch",
        "Update installation metadata does not match the signed release",
    )?;

    let mut next: BTreeMap<String, &UpdateFile> = BTreeMap::new();
    let mut next_names = HashMap::new();
    let mut previous_owned = BTreeSet::new();
    let mut paths = BTreeSet::new();
    for file in &package.files {
        if file.path == "bin/__data_directory" {
            continue;
        }
        validate_target_path(root, &file.path)?;
        paths.insert(file.path.clone());
        next_names.insert(file.path.to_lowercase(), file.path.clone());
        next.insert(file.path.clone(), file);
    }
    for file in &installed.files {
        if file.path == "bin/__data_directory" {
            continue;
        }
        validate_target_path(root, &file.path)?;
        previous_owned.insert(file.path.to_lowercase());
        paths.insert(
            next_names
                .get(&file.path.to_lowercase())
                .cloned()
                .unwrap_or_else(|| file.path.clone()),
        );
    }

    let mut entries = Vec::with_capacity(paths.len());
    for name in &paths {
        let original = root.join(name);
        let existed = original.exists();
        require(
            !existed
                || previous_owned.contains(&name.to_lowercase())
                || name == INSTALLATION_RECORD,
            "user_file_collision",
            "An update file conflicts with an existing user file",
        )?;
        let mut entry = JournalEntry {
            path: name.clone(),
            existed,
            size: None,
            sha256: None,
        };
        if existed {
            require(
                original.is_file(),
                "update_directory_collision",
                "An update file conflicts with a directory",
            )?;
            let backup = work_path(root, Path::new("backup").join(name));
            copy_and_persist(&original, &backup).map_err(|error| {
                UpdateError::new(
                    "application_backup_failed",
                    "Could not back up the current application",
                )
                .detail(error)
            })?;
            let size = fs::metadata(&original)
                .map(|metadata| metadata.len())
                .unwrap_or(u64::MAX);
            let hash = sha256_file(&original)?;
            verify_file(&backup, size, &hash)?;
            entry.size = Some(size);
            entry.sha256 = Some(hash);
        }
        entries.push(entry);
    }
    let mut journal = Journal {
        schema: 1,
        state: "applying".to_owned(),
        version: release.version.clone(),
        previous_version: installed.version,
        files: entries,
    };
    save_journal(root, &journal)?;
    let result = (|| {
        if let Some(checkpoint) = hooks.checkpoint {
            checkpoint("prepared");
        }
        for name in &paths {
            let destination = root.join(name);
            if let Some(file) = next.get(name) {
                atomic_copy(&stage.join(name), &destination)?;
                verify_file(&destination, file.size, &file.sha256)?;
            } else if destination.exists() {
                fs::remove_file(&destination).map_err(|error| {
                    io_error(
                        "obsolete_file_remove_failed",
                        "Could not remove obsolete application file",
                        error,
                    )
                })?;
            }
            if let Some(checkpoint) = hooks.checkpoint {
                checkpoint(name);
            }
        }
        platform::write_registered_version(root, &release.version)?;
        if let Some(checkpoint) = hooks.checkpoint {
            checkpoint("registry");
        }
        let ready = hooks.probe.map_or_else(
            || {
                run_with_timeout(
                    Command::new(root.join("bin/snow_shot.exe"))
                        .arg("--update-probe")
                        .arg(&release.version)
                        .stdout(Stdio::piped())
                        .stderr(Stdio::piped()),
                    Duration::from_secs(60),
                )
            },
            |probe| probe(),
        );
        require(
            ready,
            "startup_probe_failed",
            "The new application failed its startup check; restoring the previous version",
        )?;
        journal.state = "committed".to_owned();
        save_journal(root, &journal)?;
        let _ = fs::remove_file(work_path(root, "failed-version.txt"));
        fs::remove_file(work_path(root, "journal.json")).map_err(|error| {
            io_error(
                "recovery_finalize_failed",
                "Could not finalize update recovery",
                error,
            )
        })?;
        Ok(())
    })();
    if let Err(error) = result {
        restore(root)?;
        return Err(error);
    }
    Ok(())
}

pub fn prune_update_work(root: &Path) -> Result<()> {
    validate_root(root)?;
    let work = root.join(UPDATE_WORK);
    if transaction_pending(root) || !work.exists() {
        return Ok(());
    }
    let Ok(_lock) = acquire_lock(root, "Another update transaction is running") else {
        return Ok(());
    };
    let now = std::time::SystemTime::now();
    let pattern = regex::Regex::new(r"^(worker-[a-f0-9]{32}\.exe|input-[a-f0-9]{32}\.zip)$")
        .expect("valid regex");
    for entry in fs::read_dir(&work).into_iter().flatten().flatten() {
        let path = entry.path();
        let name = entry.file_name().to_string_lossy().into_owned();
        let old = entry
            .metadata()
            .and_then(|metadata| metadata.modified())
            .ok()
            .and_then(|modified| now.duration_since(modified).ok())
            .is_some_and(|age| age > Duration::from_secs(86_400));
        if old && pattern.is_match(&name) {
            no_links(&path)?;
            let _ = fs::remove_file(path);
        }
    }
    Ok(())
}

fn uninstall_work_tree_mutable(root: &Path) -> bool {
    no_links(&work_path(root, "")).is_ok()
        && selected_data_root_if_known(root)
            .is_none_or(|data| !data_collides_with_work_tree(root, &data))
}

fn uninstall_target_removable(root: &Path, relative: &str) -> bool {
    if !is_payload_relative(relative) {
        return false;
    }
    let target = root.join(relative);
    no_links(&target).is_ok()
        && selected_data_root_if_known(root)
            .is_none_or(|data| !path_starts_with_case_insensitive(&target, &data))
}

fn acquire_uninstall_lock(root: &Path) -> Result<Option<File>> {
    if !uninstall_work_tree_mutable(root) {
        return Ok(None);
    }
    if fs::create_dir_all(root.join(UPDATE_WORK)).is_err() {
        return Ok(None);
    }
    let Ok(lock) = OpenOptions::new()
        .create(true)
        .read(true)
        .write(true)
        .truncate(false)
        .open(work_path(root, "transaction.lock"))
    else {
        return Ok(None);
    };
    lock.try_lock_exclusive()
        .map_err(|error| io_error("update_lock_failed", "An update is still running", error))?;
    Ok(Some(lock))
}

fn uninstall_owned_files(root: &Path) -> Result<()> {
    let lock = acquire_uninstall_lock(root)?;
    if lock.is_some() && transaction_pending(root) {
        // Recovery only restores owned files that uninstall deletes next. A
        // damaged journal or backup is a partial installation, not a live
        // update, so it must not retain the application.
        restore(root).ok();
    }
    let record = installation_record(root).unwrap_or_default();
    for file in &record.files {
        if !uninstall_target_removable(root, &file.path) {
            continue;
        }
        let path = root.join(&file.path);
        if path.is_file() {
            fs::remove_file(&path).map_err(|error| {
                io_error(
                    "owned_file_remove_failed",
                    "Could not remove an owned application file",
                    error,
                )
            })?;
        }
    }
    drop(lock);
    if uninstall_work_tree_mutable(root) {
        clear_work_tree(root, "").ok();
    }
    Ok(())
}

pub fn uninstall(root: &Path, remove_startup: bool) -> Result<()> {
    installation_root_identity(root)?;
    if remove_startup {
        // Startup cleanup is extra work. Once the helper is running, a
        // damaged or inaccessible registration must not keep the payload
        // installed; CPack still deletes the files it knew at build time.
        platform::remove_installation_startup(root).ok();
    }
    uninstall_owned_files(root)
}

pub fn audit_release(directory: &Path, release: &UpdateRelease) -> Result<()> {
    for package in &release.packages {
        let archive = directory.join(&package.path);
        verify_file(&archive, package.size, &package.sha256)?;
        if package.kind == "installer" {
            continue;
        }
        let temporary = tempfile::tempdir().map_err(|error| {
            io_error(
                "release_audit_directory_failed",
                "Could not create release audit directory",
                error,
            )
        })?;
        extract(&archive, temporary.path(), package)?;
        let record = installation_record(temporary.path())?;
        verify_record_inventory(&record, package)?;
        require(
            record.version == release.version && record.variant == package.variant,
            "release_record_mismatch",
            "Release archive installation metadata mismatch",
        )?;
        let manifest = temporary.path().join("release-audit.json");
        write_atomic(&manifest, &release.envelope)?;
        let helper = temporary.path().join(if cfg!(windows) {
            "bin/snow-shot-updater.exe"
        } else {
            "bin/snow-shot-updater"
        });
        let trusted = run_with_timeout(
            Command::new(helper)
                .arg("--verify-release")
                .arg("--manifest")
                .arg(&manifest),
            Duration::from_secs(30),
        );
        require(
            trusted,
            "release_signature_invalid",
            "Release signature is invalid or its signing key is not trusted",
        )?;
        let succeeded = run_with_timeout(
            Command::new(temporary.path().join("bin/snow_shot.exe"))
                .arg("--update-probe")
                .arg(&release.version),
            Duration::from_secs(60),
        );
        require(
            succeeded,
            "packaged_startup_probe_failed",
            "The packaged application failed its isolated startup probe",
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use zip::write::SimpleFileOptions;

    fn hash(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }

    fn write_zip(path: &Path, entries: &[(&str, &[u8], Option<u32>)]) {
        let file = File::create(path).unwrap();
        let mut writer = zip::ZipWriter::new(file);
        for (name, bytes, permissions) in entries {
            let mut options =
                SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated);
            if let Some(permissions) = permissions {
                options = options.unix_permissions(*permissions);
            }
            writer.start_file(*name, options).unwrap();
            writer.write_all(bytes).unwrap();
        }
        writer.finish().unwrap();
    }

    fn descriptor(path: &str, bytes: &[u8]) -> UpdateFile {
        UpdateFile {
            path: path.to_owned(),
            size: bytes.len() as u64,
            sha256: hash(bytes),
        }
    }

    fn package(files: Vec<UpdateFile>) -> UpdatePackage {
        UpdatePackage {
            variant: "online".to_owned(),
            kind: "update".to_owned(),
            path: "setup/snow-shot_windows-x64-online-update.zip".to_owned(),
            size: 1,
            sha256: "0".repeat(64),
            files,
        }
    }

    #[test]
    fn installation_root_is_parent_of_binary_directory() {
        assert_eq!(
            installation_root(Path::new("C:/SnowShot/bin")),
            PathBuf::from("C:/SnowShot")
        );
    }

    #[test]
    fn archive_central_directory_is_validated_before_extraction() {
        let directory = tempfile::tempdir().unwrap();
        let archive = directory.path().join("payload.zip");
        write_zip(
            &archive,
            &[
                ("bin/known.dll", b"known", None),
                ("bin/unsigned.dll", b"unsigned", None),
            ],
        );
        let destination = directory.path().join("stage");
        fs::create_dir_all(&destination).unwrap();
        let test_package = package(vec![descriptor("bin/known.dll", b"known")]);
        assert_eq!(
            extract(&archive, &destination, &test_package)
                .unwrap_err()
                .code,
            "unsafe_archive_entry"
        );
        assert!(fs::read_dir(&destination).unwrap().next().is_none());

        let link = directory.path().join("link.zip");
        let link_file = File::create(&link).unwrap();
        let mut link_writer = zip::ZipWriter::new(link_file);
        link_writer
            .add_symlink("bin/known.dll", "target", SimpleFileOptions::default())
            .unwrap();
        link_writer.finish().unwrap();
        let link_package = package(vec![descriptor("bin/known.dll", b"target")]);
        assert!(extract(&link, &destination, &link_package).is_err());

        let missing = directory.path().join("missing.zip");
        write_zip(&missing, &[]);
        assert_eq!(
            extract(&missing, &destination, &test_package)
                .unwrap_err()
                .code,
            "incomplete_update_archive"
        );
    }

    #[test]
    fn schema_one_applying_and_committed_journals_recover() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        fs::create_dir_all(work_path(&root, "backup/bin")).unwrap();
        fs::write(root.join("bin/application.dat"), b"new").unwrap();
        fs::write(work_path(&root, "backup/bin/application.dat"), b"old").unwrap();
        fs::write(root.join("bin/added.dat"), b"added").unwrap();
        let journal = serde_json::json!({
            "schema":1,
            "state":"applying",
            "version":"2.0.0",
            "previousVersion":"1.0.0",
            "files":[
                {"path":"bin/application.dat", "existed":true, "size":3, "sha256":hash(b"old")},
                {"path":"bin/added.dat", "existed":false}
            ]
        });
        fs::write(
            work_path(&root, "journal.json"),
            serde_json::to_vec(&journal).unwrap(),
        )
        .unwrap();

        recover_transaction(&root).unwrap();
        assert_eq!(fs::read(root.join("bin/application.dat")).unwrap(), b"old");
        assert!(!root.join("bin/added.dat").exists());
        assert_eq!(
            fs::read_to_string(work_path(&root, "failed-version.txt")).unwrap(),
            "2.0.0"
        );
        assert!(!transaction_pending(&root));

        fs::write(
            work_path(&root, "journal.json"),
            br#"{"schema":1,"state":"committed","version":"2.0.0","previousVersion":"1.0.0","files":[]}"#,
        )
        .unwrap();
        recover_transaction(&root).unwrap();
        assert!(!transaction_pending(&root));
    }

    #[test]
    fn transaction_commits_and_rolls_back_after_probe_failure() {
        for probe_succeeds in [true, false] {
            let directory = tempfile::tempdir().unwrap();
            let root = directory.path().join("SnowShot");
            fs::create_dir_all(root.join("bin")).unwrap();
            let old_app = b"old application";
            let old_helper = b"old helper";
            fs::write(root.join("bin/snow_shot.exe"), old_app).unwrap();
            fs::write(root.join("bin/snow-shot-updater.exe"), old_helper).unwrap();
            let old_record = InstallationRecord {
                schema: 1,
                variant: "online".to_owned(),
                version: "1.0.0".to_owned(),
                files: vec![
                    descriptor("bin/snow_shot.exe", old_app),
                    descriptor("bin/snow-shot-updater.exe", old_helper),
                ],
            };
            fs::write(
                root.join(INSTALLATION_RECORD),
                serde_json::to_vec(&old_record).unwrap(),
            )
            .unwrap();

            let new_app = b"new application";
            let new_helper = b"new helper";
            let new_record = InstallationRecord {
                schema: 1,
                variant: "online".to_owned(),
                version: "2.0.0".to_owned(),
                files: vec![
                    descriptor("bin/snow_shot.exe", new_app),
                    descriptor("bin/snow-shot-updater.exe", new_helper),
                ],
            };
            let new_record_bytes = serde_json::to_vec(&new_record).unwrap();
            let archive = directory.path().join("update.zip");
            write_zip(
                &archive,
                &[
                    ("bin/snow_shot.exe", new_app, None),
                    ("bin/snow-shot-updater.exe", new_helper, None),
                    (INSTALLATION_RECORD, &new_record_bytes, None),
                ],
            );
            let mut update_package = package(vec![
                descriptor("bin/snow_shot.exe", new_app),
                descriptor("bin/snow-shot-updater.exe", new_helper),
                descriptor(INSTALLATION_RECORD, &new_record_bytes),
            ]);
            update_package.size = fs::metadata(&archive).unwrap().len();
            update_package.sha256 = sha256_file(&archive).unwrap();
            let release = UpdateRelease {
                version: "2.0.0".to_owned(),
                packages: vec![update_package],
                envelope: Vec::new(),
            };
            let probe = || probe_succeeds;
            let result = apply_transaction(
                &root,
                &archive,
                &release,
                TransactionHooks {
                    probe: Some(&probe),
                    checkpoint: None,
                },
            );
            if probe_succeeds {
                result.unwrap();
                assert_eq!(fs::read(root.join("bin/snow_shot.exe")).unwrap(), new_app);
                assert!(!transaction_pending(&root));
                assert!(work_path(&root, "backup/bin/snow_shot.exe").is_file());
            } else {
                assert_eq!(result.unwrap_err().code, "startup_probe_failed");
                assert_eq!(fs::read(root.join("bin/snow_shot.exe")).unwrap(), old_app);
                assert_eq!(
                    fs::read_to_string(work_path(&root, "failed-version.txt")).unwrap(),
                    "2.0.0"
                );
                assert!(!transaction_pending(&root));
            }
        }
    }

    #[test]
    fn uninstall_tolerates_missing_and_damaged_installation_records() {
        for damaged in [None, Some(b"not json".as_slice())] {
            let directory = tempfile::tempdir().unwrap();
            let root = directory.path().join("SnowShot");
            fs::create_dir_all(root.join("bin")).unwrap();
            fs::write(root.join("bin/snow_shot.exe"), b"application").unwrap();
            if let Some(bytes) = damaged {
                fs::write(root.join(INSTALLATION_RECORD), bytes).unwrap();
            }

            uninstall(&root, false).unwrap();
            assert!(root.join("bin/snow_shot.exe").is_file());
            assert!(!root.join(UPDATE_WORK).exists());
        }
    }

    #[test]
    fn uninstall_removes_owned_files_and_skips_missing_ones() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        let application = b"application";
        let helper = b"helper";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![
                descriptor("bin/snow_shot.exe", application),
                descriptor("bin/snow-shot-updater.exe", helper),
            ],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();

        uninstall(&root, false).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
        assert!(!root.join("bin/snow-shot-updater.exe").exists());
    }

    #[test]
    fn uninstall_tolerates_damaged_recovery_journal() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        fs::create_dir_all(work_path(&root, "backup/bin")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        fs::write(root.join("bin/user-data.dat"), b"user data").unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![descriptor("bin/snow_shot.exe", application)],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();
        fs::write(work_path(&root, "journal.json"), b"damaged journal").unwrap();

        uninstall(&root, false).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
        assert!(root.join("bin/user-data.dat").is_file());
        assert!(!root.join(UPDATE_WORK).exists());
    }

    #[test]
    fn uninstall_skips_entries_colliding_with_the_data_directory() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        // The selected data directory covers one claimed file; removing that
        // entry must not block removal of the remaining owned files.
        fs::write(root.join("bin/__data_directory"), b"missing-data").unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![
                descriptor("bin/snow_shot.exe", application),
                descriptor("bin/missing-data/plugin.dll", b"plugin"),
            ],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();

        uninstall(&root, false).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
        assert!(!root.join(UPDATE_WORK).exists());
    }

    #[test]
    fn validate_target_path_rejects_files_under_an_existing_data_directory() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin/my-data")).unwrap();
        fs::write(root.join("bin/__data_directory"), b"my-data").unwrap();
        assert_eq!(
            validate_target_path(&root, "bin/my-data/plugin.dll")
                .unwrap_err()
                .code,
            "selected_data_collision"
        );
    }

    #[test]
    fn uninstall_rejects_an_invalid_installation_root() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("x");
        fs::create_dir(&root).unwrap();
        assert_eq!(
            uninstall(&root, false).unwrap_err().code,
            "invalid_installation_root"
        );
        assert_eq!(
            validate_root(&root).unwrap_err().code,
            "invalid_installation_root"
        );
    }

    #[test]
    fn uninstall_tolerates_a_damaged_data_directory_marker() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        fs::write(root.join("bin/__data_directory"), [0xff]).unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![descriptor("bin/snow_shot.exe", application)],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();

        assert_eq!(
            validate_root(&root).unwrap_err().code,
            "data_directory_invalid"
        );
        uninstall(&root, true).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
    }

    #[test]
    fn uninstall_skips_a_work_tree_that_collides_with_user_data() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        fs::create_dir_all(work_path(&root, "user")).unwrap();
        fs::write(work_path(&root, "user/photo.png"), b"photo").unwrap();
        fs::write(root.join("bin/__data_directory"), b"../.snow-shot-update").unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![descriptor("bin/snow_shot.exe", application)],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();

        assert_eq!(
            validate_root(&root).unwrap_err().code,
            "selected_data_collision"
        );
        uninstall(&root, false).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
        assert_eq!(
            fs::read(work_path(&root, "user/photo.png")).unwrap(),
            b"photo"
        );
    }

    // LockFileEx contends per handle; POSIX process-wide locks would not.
    #[cfg(windows)]
    #[test]
    fn uninstall_blocks_while_an_update_lock_is_held() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![descriptor("bin/snow_shot.exe", application)],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();
        let _lock = acquire_lock(&root, "held").unwrap();

        assert_eq!(
            uninstall(&root, false).unwrap_err().code,
            "update_lock_failed"
        );
        assert!(root.join("bin/snow_shot.exe").is_file());
    }

    #[test]
    fn uninstall_skips_directories_claimed_as_owned_files() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("SnowShot");
        fs::create_dir_all(root.join("bin/plugin.dll")).unwrap();
        let application = b"application";
        fs::write(root.join("bin/snow_shot.exe"), application).unwrap();
        let record = InstallationRecord {
            schema: 1,
            variant: "online".to_owned(),
            version: "1.0.0".to_owned(),
            files: vec![
                descriptor("bin/snow_shot.exe", application),
                descriptor("bin/plugin.dll", b"plugin"),
            ],
        };
        fs::write(
            root.join(INSTALLATION_RECORD),
            serde_json::to_vec(&record).unwrap(),
        )
        .unwrap();

        uninstall(&root, false).unwrap();
        assert!(!root.join("bin/snow_shot.exe").exists());
        assert!(root.join("bin/plugin.dll").is_dir());
    }
}

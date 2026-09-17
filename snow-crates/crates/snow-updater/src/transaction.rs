//! The journaled apply/recover/uninstall transaction over owned files.

use crate::contract::{UpdateFile, UpdatePackage, UpdateRelease, verify_file};
use crate::crypto::sha256_file;
use crate::errors::{Error, Result, require};
use crate::installation::{
    InstallationRecord, RECORD_FILE, installation_record, path_at, transaction_pending,
    validate_installation_root, validate_target_path, work_path,
};
use crate::platform::fsx::{
    self, TransactionLock, atomic_copy, clear_work_tree, no_links, write_atomic,
};
use crate::platform::processx;
use crate::platform::registryx::write_registered_version;
use crate::semver::compare_versions;
use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};
use std::path::Path;

/// Failure-injection hooks used by the crash-recovery tests.
#[derive(Default)]
pub struct TransactionHooks {
    pub probe: Option<Box<dyn FnMut() -> bool>>,
    #[allow(clippy::type_complexity)]
    pub checkpoint: Option<Box<dyn FnMut(&str)>>,
}

fn save_journal(root: &Path, journal: &Value) -> Result<()> {
    write_atomic(
        &work_path(root, "journal.json"),
        journal.to_string().as_bytes(),
    )
}

fn verify_record_inventory(record: &InstallationRecord, package: &UpdatePackage) -> Result<()> {
    let mut expected: HashMap<String, &UpdateFile> = package
        .files
        .iter()
        .filter(|file| file.path != RECORD_FILE)
        .map(|file| (file.path.clone(), file))
        .collect();
    for file in &record.files {
        let found = expected.get(&file.path);
        require(
            found.is_some_and(|expected| {
                expected.size == file.size && expected.sha256 == file.sha256
            }),
            "Update installation metadata does not match the signed release",
        )?;
        expected.remove(&file.path);
    }
    require(
        expected.is_empty(),
        "Update installation metadata does not match the signed release",
    )
}

/// Restores the journaled pre-update state; also finalizes committed
/// journals left behind by a power loss after commit.
fn restore(root: &Path) -> Result<()> {
    let bytes = fsx::read_limited(&work_path(root, "journal.json"), 8 * 1024 * 1024)?;
    let journal: Value = serde_json::from_slice(&bytes)
        .or(Err(Error::fixed("Unsupported update recovery journal")))?;
    require(
        journal.get("schema").and_then(Value::as_i64) == Some(1),
        "Unsupported update recovery journal",
    )?;
    if journal.get("state").and_then(Value::as_str) == Some("committed") {
        require(
            fsx::remove_file(&work_path(root, "journal.json")),
            "Could not finalize committed update state",
        )?;
        return Ok(());
    }
    let Some(entries) = journal.get("files").and_then(Value::as_array) else {
        return Err(Error::fixed("Unsupported update recovery journal"));
    };
    // Validate every backup before restoring anything. Never consume
    // arbitrary journal paths.
    for entry in entries {
        let name = entry
            .get("path")
            .and_then(Value::as_str)
            .unwrap_or_default();
        validate_target_path(root, name)?;
        if entry.get("existed").and_then(Value::as_bool) == Some(true) {
            let backup = work_path(root, &format!("backup/{name}"));
            no_links(&backup)?;
            let size = entry.get("size").and_then(Value::as_i64).unwrap_or(-1);
            let hash = entry
                .get("sha256")
                .and_then(Value::as_str)
                .unwrap_or_default();
            verify_file(&backup, size, hash)?;
        }
    }
    for entry in entries {
        let name = entry
            .get("path")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let destination = path_at(root, name);
        if entry.get("existed").and_then(Value::as_bool) == Some(true) {
            atomic_copy(&work_path(root, &format!("backup/{name}")), &destination)?;
        } else if fsx::file_exists(&destination) {
            require(
                fsx::remove_file(&destination),
                "Could not remove an incomplete update file",
            )?;
        }
    }
    write_registered_version(
        root,
        journal
            .get("previousVersion")
            .and_then(Value::as_str)
            .unwrap_or_default(),
    )?;
    let version = journal
        .get("version")
        .and_then(Value::as_str)
        .unwrap_or_default();
    write_atomic(&work_path(root, "failed-version.txt"), version.as_bytes())?;
    require(
        fsx::remove_file(&work_path(root, "journal.json")),
        "Could not finalize update recovery",
    )
}

pub fn recover_transaction(root: &Path) -> Result<()> {
    validate_installation_root(root)?;
    let Some(_lock) = TransactionLock::try_lock(&work_path(root, "transaction.lock")) else {
        return Err(Error::fixed("Another update transaction is running"));
    };
    if transaction_pending(root) {
        restore(root)?;
    }
    Ok(())
}

pub fn apply_transaction(
    root: &Path,
    archive: &Path,
    release: &UpdateRelease,
    hooks: TransactionHooks,
) -> Result<()> {
    let mut hooks = hooks;
    validate_installation_root(root)?;
    fsx::create_dir_all(&work_path(root, ""))
        .map_err(|_| Error::fixed("Could not create update work directory"))?;
    let Some(_lock) = TransactionLock::try_lock(&work_path(root, "transaction.lock")) else {
        return Err(Error::fixed("Another update transaction is running"));
    };
    if transaction_pending(root) {
        restore(root)?;
    }
    let installed = installation_record(root)?;
    require(
        matches!(
            compare_versions(&release.version, &installed.version),
            Ok(std::cmp::Ordering::Greater)
        ),
        "The update must be newer than the installed release",
    )?;
    let package = release.update_package(&installed.variant)?;
    verify_file(archive, package.size, &package.sha256)?;
    let mut needed = package.size as u128;
    for file in &package.files {
        needed += file.size as u128 * 3;
    }
    let available = fsx::free_space(root)
        .ok_or_else(|| Error::fixed("Not enough free space to stage and recover this update"))?;
    require(
        available as u128 > needed + 64 * 1024 * 1024,
        "Not enough free space to stage and recover this update",
    )?;
    clear_work_tree(&work_path(root, "stage"))?;
    clear_work_tree(&work_path(root, "backup"))?;
    crate::zipentry::extract(archive, &work_path(root, "stage"), package)?;
    let staged_record = installation_record(&work_path(root, "stage"))?;
    verify_record_inventory(&staged_record, package)?;
    require(
        staged_record.version == release.version && staged_record.variant == installed.variant,
        "Update installation metadata does not match the signed release",
    )?;
    let mut next: HashMap<String, UpdateFile> = HashMap::new();
    let mut paths: HashSet<String> = HashSet::new();
    let mut previous_owned: HashSet<String> = HashSet::new();
    let mut next_names: HashMap<String, String> = HashMap::new();
    for file in &package.files {
        if file.path == "bin/__data_directory" {
            continue;
        }
        validate_target_path(root, &file.path)?;
        paths.insert(file.path.clone());
        next.insert(file.path.clone(), file.clone());
        next_names.insert(file.path.to_lowercase(), file.path.clone());
    }
    for file in &parse_file_inventory_entries(&installed)? {
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
    let mut sorted_paths: Vec<String> = paths.into_iter().collect();
    sorted_paths.sort();
    let mut entries = Vec::with_capacity(sorted_paths.len());
    for name in &sorted_paths {
        let original = path_at(root, name);
        let existed = fsx::file_exists(&original);
        require(
            !existed || previous_owned.contains(&name.to_lowercase()) || name == RECORD_FILE,
            "An update file conflicts with an existing user file",
        )?;
        let mut entry = json!({ "path": name, "existed": existed });
        if existed {
            require(
                fsx::is_file(&original),
                "An update file conflicts with a directory",
            )?;
            let backup = work_path(root, &format!("backup/{name}"));
            if let Some(parent) = backup.parent() {
                fsx::create_dir_all(parent)
                    .map_err(|_| Error::fixed("Could not back up the current application"))?;
            }
            std::fs::copy(&original, &backup)
                .map_err(|_| Error::fixed("Could not back up the current application"))?;
            let size = fsx::file_size(&original) as i64;
            let hash = sha256_file(&original)?;
            entry["size"] = json!(size);
            entry["sha256"] = json!(hash);
            verify_file(&backup, size, entry["sha256"].as_str().unwrap_or_default())?;
            fsx::persist_file(&backup)?;
        }
        entries.push(entry);
    }
    let mut journal = json!({
        "schema": 1,
        "state": "applying",
        "version": release.version,
        "previousVersion": installed.version,
        "files": entries,
    });
    save_journal(root, &journal)?;
    // Panics from injected hooks (and any unexpected bug) must restore the
    // journaled state before unwinding, exactly like the historical C++
    // exception path; the release profile aborts on panic and leaves the
    // journal for the next startup's recovery instead.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| -> Result<()> {
        if let Some(checkpoint) = &mut hooks.checkpoint {
            checkpoint("prepared");
        }
        for name in &sorted_paths {
            if let Some(file) = next.get(name) {
                atomic_copy(
                    &work_path(root, &format!("stage/{name}")),
                    &path_at(root, name),
                )?;
                verify_file(&path_at(root, name), file.size, &file.sha256)?;
            } else if fsx::file_exists(&path_at(root, name)) {
                require(
                    fsx::remove_file(&path_at(root, name)),
                    "Could not remove obsolete application file",
                )?;
            }
            if let Some(checkpoint) = &mut hooks.checkpoint {
                checkpoint(name);
            }
        }
        write_registered_version(root, &release.version)?;
        if let Some(checkpoint) = &mut hooks.checkpoint {
            checkpoint("registry");
        }
        let ready = match &mut hooks.probe {
            Some(probe) => probe(),
            None => {
                processx::spawn_and_wait(
                    &path_at(root, "bin/snow_shot.exe"),
                    &["--update-probe".to_owned(), release.version.clone()],
                    60000,
                ) == Some(0)
            }
        };
        require(
            ready,
            "The new application failed its startup check; restoring the previous version",
        )?;
        journal["state"] = json!("committed");
        save_journal(root, &journal)?;
        let _ = fsx::remove_file(&work_path(root, "failed-version.txt"));
        // Keep one complete backup. A committed journal is harmless after
        // power loss and is finalized by the next recovery.
        let _ = fsx::remove_file(&work_path(root, "journal.json"));
        Ok(())
    }));
    match result {
        Ok(Ok(())) => Ok(()),
        Ok(Err(error)) => {
            let _ = restore(root);
            Err(error)
        }
        Err(panic) => {
            let _ = restore(root);
            std::panic::resume_unwind(panic);
        }
    }
}

fn parse_file_inventory_entries(record: &InstallationRecord) -> Result<Vec<UpdateFile>> {
    // The installed record's inventory was already parsed and validated on
    // read; reuse it directly.
    Ok(record.files.clone())
}

pub fn uninstall_owned_files(root: &Path) -> Result<()> {
    validate_installation_root(root)?;
    fsx::create_dir_all(&work_path(root, ""))
        .map_err(|_| Error::fixed("Could not create uninstall lock directory"))?;
    let mut lock = match TransactionLock::try_lock(&work_path(root, "transaction.lock")) {
        Some(lock) => lock,
        None => return Err(Error::fixed("An update is still running")),
    };
    if transaction_pending(root) {
        restore(root)?;
    }
    let record = installation_record(root)?;
    for file in &record.files {
        if file.path != "bin/__data_directory" {
            validate_target_path(root, &file.path)?;
        }
    }
    for file in &record.files {
        if file.path == "bin/__data_directory" {
            continue;
        }
        let path = path_at(root, &file.path);
        if fsx::file_exists(&path) {
            require(
                fsx::remove_file(&path),
                "Could not remove an owned application file",
            )?;
        }
    }
    lock.unlock();
    clear_work_tree(&work_path(root, ""))
}

/// The release publisher's pre-upload audit: package hashes, extraction,
/// installation metadata, the packaged helper's trust, and startup probes.
pub fn audit_release(directory: &Path, release: &UpdateRelease) -> Result<()> {
    for package in &release.packages {
        let archive = directory.join(&package.path);
        verify_file(&archive, package.size, &package.sha256)?;
        if package.kind == "installer" {
            continue;
        }
        let temporary =
            std::env::temp_dir().join(format!("snow-shot-audit-{}", uuid::Uuid::new_v4().simple()));
        let _audit = AuditDirectory(temporary.clone());
        fsx::create_dir_all(&temporary)
            .map_err(|_| Error::fixed("Could not create release audit directory"))?;
        crate::zipentry::extract(&archive, &temporary, package)?;
        let record = installation_record(&temporary)?;
        verify_record_inventory(&record, package)?;
        require(
            record.version == release.version && record.variant == package.variant,
            "Release archive installation metadata mismatch",
        )?;
        let manifest = temporary.join("release-audit.json");
        write_atomic(&manifest, &release.envelope)?;
        let trusted = processx::spawn_and_wait(
            &temporary.join("bin/snow-shot-updater.exe"),
            &[
                "--verify-release".to_owned(),
                "--manifest".to_owned(),
                manifest.to_string_lossy().into_owned(),
            ],
            30000,
        ) == Some(0);
        require(
            trusted,
            "Release signature is invalid or its signing key is not trusted",
        )?;
        let succeeded = processx::spawn_and_wait(
            &temporary.join("bin/snow_shot.exe"),
            &["--update-probe".to_owned(), release.version.clone()],
            60000,
        ) == Some(0);
        require(
            succeeded,
            "The packaged application failed its isolated startup probe",
        )?;
    }
    Ok(())
}

struct AuditDirectory(std::path::PathBuf);

impl Drop for AuditDirectory {
    fn drop(&mut self) {
        let _ = clear_work_tree(&self.0);
        let _ = std::fs::remove_dir(&self.0);
    }
}

/// `--crash-apply` support for the crash-recovery tests: applies with a
/// checkpoint hook that terminates the process without unwinding.
pub fn apply_transaction_crashing_at(
    root: &Path,
    archive: &Path,
    release: &UpdateRelease,
    point: &str,
) -> Result<()> {
    let owned = point.to_owned();
    apply_transaction(
        root,
        archive,
        release,
        TransactionHooks {
            probe: Some(Box::new(|| true)),
            checkpoint: Some(Box::new(move |current| {
                if current == owned {
                    processx::terminate_self(77);
                }
            })),
        },
    )
}

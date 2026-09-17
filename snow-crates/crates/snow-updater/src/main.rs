//! The standalone helper CLI. Modes and wire behavior replicate the previous
//! implementation exactly; see `docs/snow-shot-releases.md`.

#![windows_subsystem = "windows"]

use snow_updater::contract::{METADATA_LIMIT, UpdateRelease, verify_release};
use snow_updater::errors::{Error, Result, require};
use snow_updater::installation::{
    installation_record, prune_update_work, transaction_pending, validate_installation_root,
    validate_target_path, work_path,
};
use snow_updater::platform::{desktop, fsx, peer, pipes, processx, registryx, startup};
use snow_updater::transaction::{
    TransactionHooks, apply_transaction, audit_release, recover_transaction, uninstall_owned_files,
};
use std::ffi::{OsStr, OsString};
use std::path::PathBuf;
use std::time::{Duration, Instant};

struct Arguments(Vec<OsString>);

impl Arguments {
    fn contains(&self, name: &str) -> bool {
        self.0.iter().any(|argument| argument == OsStr::new(name))
    }

    fn first(&self) -> Option<&OsStr> {
        self.0.first().map(|argument| argument.as_os_str())
    }

    fn option(&self, name: &str) -> Result<OsString> {
        let index = self
            .0
            .iter()
            .position(|argument| argument == OsStr::new(name))
            .ok_or_else(|| Error::fixed("Missing updater argument"))?;
        self.0
            .get(index + 1)
            .cloned()
            .ok_or_else(|| Error::fixed("Missing updater argument"))
    }

    fn option_path(&self, name: &str) -> Result<PathBuf> {
        Ok(PathBuf::from(self.option(name)?))
    }

    fn option_text(&self, name: &str) -> Result<String> {
        Ok(self.option(name)?.to_string_lossy().into_owned())
    }

    fn replace_mode(&self, mode: &str) -> Vec<OsString> {
        let mut replaced = self.0.clone();
        if let Some(first) = replaced.first_mut() {
            *first = OsString::from(mode);
        }
        replaced
    }
}

/// Removes coordinator copies older than a day without recursive sweeps.
fn prune_coordinators() {
    let temporary = std::env::temp_dir();
    let Ok(entries) = std::fs::read_dir(&temporary) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let generated = name.len() == "snow-shot-updater-XXXXXX".len()
            && name.starts_with("snow-shot-updater-")
            && name["snow-shot-updater-".len()..]
                .chars()
                .all(|c| c.is_ascii_alphanumeric());
        if !generated
            || entry
                .path()
                .symlink_metadata()
                .is_ok_and(|meta| meta.file_type().is_symlink())
        {
            continue;
        }
        if !fsx::last_modified_before(&entry.path(), 86400) {
            continue;
        }
        if !fsx::attributes_without_reparse(&entry.path()) {
            continue;
        }
        let executable = entry.path().join("coordinator.exe");
        if executable
            .symlink_metadata()
            .is_ok_and(|meta| meta.file_type().is_symlink())
        {
            continue;
        }
        // Non-recursive: an unexpected user-created file always preserves
        // its directory.
        if fsx::remove_file(&executable) {
            let _ = std::fs::remove_dir(entry.path());
        }
    }
}

/// Peer identity rules for helper-to-helper connections.
fn verify_coordinator(stream: &pipes::PipeStream, pipe: &str, args: &Arguments) -> Result<()> {
    let worker_pipe = pipe.ends_with("-worker");
    let target = args.option_path("--target")?;
    let expected = if worker_pipe {
        target.join("bin/snow-shot-updater.exe")
    } else {
        target.join("bin/snow_shot.exe")
    };
    let parent = if worker_pipe {
        0
    } else {
        args.option_text("--parent")?.parse::<u32>().unwrap_or(0)
    };
    require(
        peer::verify_local_peer(stream, true, &expected, worker_pipe, parent, None),
        "The update coordinator identity could not be verified",
    )
}

fn send_line(pipe: &str, line: &[u8]) -> Result<()> {
    let stream = pipes::connect(pipe, 10000)?;
    verify_coordinator(&stream, pipe, &invocation())?;
    stream.write_line(line, 5000)
}

fn exchange_line(pipe: &str, line: &[u8]) -> Result<Vec<u8>> {
    let stream = pipes::connect(pipe, 10000)?;
    verify_coordinator(&stream, pipe, &invocation())?;
    stream.write_line(line, 5000)?;
    stream.read_line(30000)
}

/// The raw process arguments, reconstructed lazily for peer verification.
fn invocation() -> Arguments {
    Arguments(std::env::args_os().skip(1).collect())
}

fn read_limited(path: &std::path::Path) -> Result<Vec<u8>> {
    fsx::read_limited(path, METADATA_LIMIT)
}

fn worker(args: &Arguments) -> Result<i32> {
    let root = args.option_path("--target")?;
    validate_installation_root(&root)?;
    let pipe = format!("{}-worker", args.option_text("--pipe")?);
    let parent_text = args.option_text("--parent")?;
    let recovery = args.contains("--recovery");
    let parent_id = parent_text
        .parse::<u32>()
        .map_err(|_| Error::fixed("Could not open the application process"))?;
    require(
        processx::process_image(parent_id).is_some_and(|image| {
            snow_updater::paths::paths_ci_eq(
                &snow_updater::paths::clean_path(&image.to_string_lossy()),
                &snow_updater::paths::clean_path(&root.join("bin/snow_shot.exe").to_string_lossy()),
            )
        }),
        "The update target does not match its application process",
    )?;
    let mut release = UpdateRelease {
        version: String::new(),
        packages: Vec::new(),
        envelope: Vec::new(),
    };
    let mut archive = Option::<PathBuf>::None;
    if !recovery {
        release = verify_release(&read_limited(&args.option_path("--manifest")?)?, None)?;
        let record = installation_record(&root)?;
        let package = release.update_package(&record.variant)?;
        require(
            matches!(
                snow_updater::semver::compare_versions(&release.version, &record.version),
                Ok(std::cmp::Ordering::Greater)
            ),
            "The release is not newer than this installation",
        )?;
        let staged = work_path(
            &root,
            &format!("input-{}.zip", uuid::Uuid::new_v4().simple()),
        );
        std::fs::copy(args.option_path("--archive")?, &staged)
            .map_err(|_| Error::fixed("Could not stage the verified update package"))?;
        snow_updater::contract::verify_file(&staged, package.size, &package.sha256)?;
        archive = Some(staged);
    }
    let answer = exchange_line(&pipe, b"ready")?;
    require(
        answer.as_slice() == b"go",
        "The application cancelled the update handoff",
    )?;
    require(
        processx::wait_for_exit(parent_id, 45000),
        "The application did not exit; the update was cancelled",
    )?;
    let outcome = if recovery {
        recover_transaction(&root)
    } else {
        apply_transaction(
            &root,
            archive.as_deref().unwrap(),
            &release,
            TransactionHooks::default(),
        )
    };

    match outcome {
        Ok(()) => {
            send_line(&pipe, b"success")?;
            Ok(0)
        }
        Err(error) => {
            let mut failure = b"failed:".to_vec();
            failure.extend_from_slice(error.message().as_bytes());
            let _ = send_line(&pipe, &failure);
            Err(error)
        }
    }
}

const PRE_HANDOFF_WATCHDOG: u64 = 180_000;
const POST_HANDOFF_WATCHDOG: u64 = 15 * 60_000;

fn broker(args: &Arguments) -> Result<i32> {
    let root = args.option_path("--target")?;
    let worker_digest = {
        let image = processx::current_process_image();
        snow_updater::crypto::sha256_file(&image)
            .map_err(|_| Error::fixed("Could not verify the running update coordinator"))?
    };
    let pipe = args.option_text("--pipe")?;
    let mut server = pipes::PrivilegedPipeServer::listen(&format!("{pipe}-worker"))?;
    let mut handed_off = false;
    let result_path = args.option_path("--result")?;
    let installed_helper = root.join("bin/snow-shot-updater.exe");
    require(
        processx::start_detached(&installed_helper, &args.replace_mode("--bootstrap"), &root),
        "Could not launch the installed update helper",
    )?;
    let deadline = Instant::now() + Duration::from_millis(PRE_HANDOFF_WATCHDOG);
    loop {
        let remaining = deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .min(u32::MAX as u128) as u32;
        let worker_stream = match server.accept(remaining) {
            Ok(stream) => stream,
            Err(_) => return Ok(broker_timeout(&pipe, &result_path, &root, handed_off)),
        };
        if !peer::verify_local_peer(
            &worker_stream,
            false,
            &installed_helper,
            true,
            0,
            Some(&worker_digest),
        ) {
            continue;
        }
        let first = match worker_stream.read_line(30000) {
            Ok(line) => line,
            Err(_) => return Ok(broker_timeout(&pipe, &result_path, &root, handed_off)),
        };
        if first.as_slice() != b"ready" {
            return Ok(finish_public(
                &first,
                &pipe,
                &result_path,
                &root,
                handed_off,
            ));
        }
        let answer = match exchange_line(&pipe, b"ready") {
            Ok(answer) => answer,
            Err(_) => {
                return Ok(finish_public(
                    b"failed:Application coordinator disconnected",
                    &pipe,
                    &result_path,
                    &root,
                    handed_off,
                ));
            }
        };
        let _ = worker_stream.write_line(&answer, 5000);
        if answer.as_slice() != b"go" {
            return Ok(finish_public(
                b"failed:Application cancelled the update handoff",
                &pipe,
                &result_path,
                &root,
                handed_off,
            ));
        }
        handed_off = true;
        drop(worker_stream);
        // The worker reports its final status on a fresh connection. A slow
        // or stuck worker may still own the transaction, so never launch a
        // partially replaced app on a watchdog deadline; a later manual
        // launch enters recovery instead.
        let deadline = Instant::now() + Duration::from_millis(POST_HANDOFF_WATCHDOG);
        let remaining = deadline
            .saturating_duration_since(Instant::now())
            .as_millis()
            .min(u32::MAX as u128) as u32;
        return match server.accept(remaining) {
            Ok(status_stream) => {
                let verified = peer::verify_local_peer(
                    &status_stream,
                    false,
                    &installed_helper,
                    true,
                    0,
                    Some(&worker_digest),
                );
                if !verified {
                    return Ok(broker_timeout(&pipe, &result_path, &root, handed_off));
                }
                match status_stream.read_line(30000) {
                    Ok(status) => Ok(finish_public(
                        &status,
                        &pipe,
                        &result_path,
                        &root,
                        handed_off,
                    )),
                    Err(_) => Ok(broker_timeout(&pipe, &result_path, &root, handed_off)),
                }
            }
            Err(_) => Ok(broker_timeout(&pipe, &result_path, &root, handed_off)),
        };
    }
}

fn broker_timeout(
    pipe: &str,
    result: &std::path::Path,
    root: &std::path::Path,
    handed_off: bool,
) -> i32 {
    let status = b"failed:The update helper timed out";
    if handed_off {
        let _ = fsx::write_atomic(result, status);
        return 1;
    }
    finish_public(status, pipe, result, root, handed_off)
}

fn finish_public(
    status: &[u8],
    pipe: &str,
    result: &std::path::Path,
    root: &std::path::Path,
    handed_off: bool,
) -> i32 {
    if !handed_off {
        let _ = send_line(pipe, status);
    } else {
        let _ = fsx::write_atomic(result, status);
        if !transaction_pending(root) {
            processx::start_detached(
                &root.join("bin/snow_shot.exe"),
                &[OsString::from("--show-main-window")],
                root,
            );
        }
    }
    i32::from(status != b"success")
}

fn bootstrap(args: &Arguments, mode: &str) -> Result<i32> {
    let root = args.option_path("--target")?;
    validate_installation_root(&root)?;
    installation_record(&root)?;
    validate_target_path(&root, "bin/snow-shot-updater.exe")?;
    if mode == "--elevated" {
        registryx::validate_registered_target(&root)?;
    }
    if !fsx::writable(&root) {
        registryx::validate_registered_target(&root)?;
        require(
            mode != "--elevated"
                && processx::launch_elevated(
                    &processx::current_process_image(),
                    &args.replace_mode("--elevated"),
                )?,
            "Update permission was declined or could not be obtained",
        )?;
        return Ok(0);
    }
    fsx::create_dir_all(&work_path(&root, ""))
        .map_err(|_| Error::fixed("Could not create update worker directory"))?;
    prune_update_work(&root)?;
    // The unique worker path stays outside the payload and inherits the
    // installation's permissions.
    let executable = work_path(
        &root,
        &format!("worker-{}.exe", uuid::Uuid::new_v4().simple()),
    );
    let image = processx::current_process_image();
    require(
        std::fs::copy(&image, &executable).is_ok()
            && processx::start_detached(&executable, &args.replace_mode("--worker"), &root),
        "Could not launch update worker",
    )?;
    Ok(0)
}

fn launch(args: &Arguments) -> Result<i32> {
    prune_coordinators();
    let mut unique = None;
    for _ in 0..64 {
        let suffix: String = uuid::Uuid::new_v4()
            .simple()
            .to_string()
            .chars()
            .take(6)
            .collect();
        let candidate = std::env::temp_dir().join(format!("snow-shot-updater-{suffix}"));
        if std::fs::create_dir(&candidate).is_ok() {
            unique = Some(candidate);
            break;
        }
    }
    let temporary =
        unique.ok_or_else(|| Error::fixed("Could not create updater coordinator directory"))?;
    let executable = temporary.join("coordinator.exe");
    let image = processx::current_process_image();
    if std::fs::copy(&image, &executable).is_err() {
        let _ = std::fs::remove_dir_all(&temporary);
        return Err(Error::fixed("Could not copy updater coordinator"));
    }
    if !processx::start_detached(&executable, &args.replace_mode("--broker"), &temporary) {
        let _ = std::fs::remove_dir_all(&temporary);
        return Err(Error::fixed("Could not launch updater coordinator"));
    }
    Ok(0)
}

fn run(args: &Arguments) -> Result<i32> {
    let mode = args
        .first()
        .map(|mode| mode.to_string_lossy().into_owned())
        .ok_or_else(|| Error::fixed("Missing updater operation"))?;
    if mode == "--verify-release" {
        let manifest = read_limited(&args.option_path("--manifest")?)?;
        verify_release(&manifest, None)?;
        return Ok(0);
    }
    if mode == "--audit-release" {
        let release = verify_release(&read_limited(&args.option_path("--manifest")?)?, None)?;
        audit_release(&args.option_path("--directory")?, &release)?;
        return Ok(0);
    }
    if mode == "--crash-apply" {
        // Test-only: terminate at a journal checkpoint to exercise recovery.
        let root = args.option_path("--crash-root")?;
        let archive = args.option_path("--crash-archive")?;
        let keys = String::from_utf8_lossy(&read_limited(&args.option_path("--crash-keys")?)?)
            .into_owned();
        let release = verify_release(
            &read_limited(&args.option_path("--crash-manifest")?)?,
            Some(&keys),
        )?;
        snow_updater::transaction::apply_transaction_crashing_at(
            &root,
            &archive,
            &release,
            &args.option_text("--crash-point")?,
        )?;
        return Ok(2);
    }
    let root = snow_updater::paths::clean_path(&args.option_path("--target")?.to_string_lossy());
    let root = PathBuf::from(&root);
    if mode == "--migrate-startup" {
        validate_installation_root(&root)?;
        let previous = args.option_path("--previous")?;
        require(previous.is_absolute(), "Invalid previous installation path")?;
        startup::migrate_installation_startup(&previous, &root)?;
        return Ok(0);
    }
    if mode == "--launch-desktop" {
        validate_installation_root(&root)?;
        return Ok(i32::from(!desktop::launch_on_interactive_desktop(
            &root.join("bin/snow_shot.exe"),
        )));
    }
    if mode == "--uninstall" {
        validate_installation_root(&root)?;
        if !args.contains("--upgrade") {
            startup::remove_installation_startup(&root)?;
        }
        uninstall_owned_files(&root)?;
        return Ok(0);
    }
    if mode == "--launch" {
        return launch(args);
    }
    if mode == "--broker" {
        return broker(args);
    }
    if mode == "--bootstrap" || mode == "--elevated" {
        return bootstrap(args, &mode);
    }
    if mode == "--worker" {
        return worker(args);
    }
    Err(Error::fixed("Unknown updater operation"))
}

fn main() {
    let args = invocation();
    let result = run(&args);
    let code = match result {
        Ok(code) => code,
        Err(error) => {
            let text = error.message();
            eprintln!("{text}");
            if args.contains("--pipe") {
                let mode = args
                    .first()
                    .map(|mode| mode.to_string_lossy().into_owned())
                    .unwrap_or_default();
                let suffix = if mode == "--launch" { "" } else { "-worker" };
                let pipe = format!("{}{suffix}", args.option_text("--pipe").unwrap_or_default());
                let mut failure = b"failed:".to_vec();
                failure.extend_from_slice(text.as_bytes());
                let _ = send_line(&pipe, &failure);
            }
            1
        }
    };
    std::process::exit(code);
}

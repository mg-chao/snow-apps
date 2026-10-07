#![cfg(windows)]

use snow_shot_updater::{edition, fsutil, transaction};
use std::{fs, path::Path, process::Command};

fn install(root: &Path, data: &Path) {
    fs::create_dir_all(root.join("bin")).unwrap();
    fs::write(root.join(edition::APP_PATH), b"application payload").unwrap();
    fs::copy(
        env!("CARGO_BIN_EXE_snow-shot-updater"),
        root.join(edition::UPDATER_PATH),
    )
    .unwrap();
    fs::write(root.join(edition::MARKER_PATH), data.to_str().unwrap()).unwrap();
    let files = [edition::APP_PATH, edition::UPDATER_PATH]
        .into_iter()
        .map(|relative| {
            let path = root.join(relative);
            snow_shot_updater::contract::UpdateFile {
                path: relative.to_owned(),
                size: fs::metadata(&path).unwrap().len(),
                sha256: fsutil::sha256_file(&path).unwrap(),
            }
        })
        .collect();
    let record = transaction::InstallationRecord {
        schema: 1,
        platform: Some(edition::PLATFORM.to_owned()),
        product: edition::PRODUCT.to_owned(),
        variant: "online".to_owned(),
        version: "1.2.3".to_owned(),
        files,
    };
    fs::write(
        root.join(edition::INSTALLATION_RECORD),
        serde_json::to_vec(&record).unwrap(),
    )
    .unwrap();
}

fn uninstall(helper: &Path, root: &Path) {
    let output = Command::new(helper)
        .args(["--uninstall", "--upgrade", "--target"])
        .arg(root)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "uninstall failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn installed_helper_defers_its_own_removal_until_it_exits() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("Snow Shot 安");
    let data = directory.path().join("custom configuration 配置");
    fs::create_dir(&data).unwrap();
    fs::write(data.join("config.json"), b"preserve custom configuration").unwrap();
    install(&root, &data);
    let installed = root.join(edition::UPDATER_PATH);

    // OwnedCleanup.nsh uses the installed helper if copying it to $PLUGINSDIR fails.
    uninstall(&installed, &root);

    assert!(!root.join(edition::APP_PATH).exists());
    assert!(installed.is_file());
    // CPack owns this file and deletes it after ExecWait observes the helper's exit.
    fs::remove_file(&installed).unwrap();
    assert_eq!(
        fs::read(data.join("config.json")).unwrap(),
        b"preserve custom configuration"
    );
}

#[test]
fn copied_helper_removes_the_installed_helper_and_preserves_custom_data() {
    let directory = tempfile::tempdir().unwrap();
    for name in ["external configuration 配置", "missing configuration 配置"] {
        let root = directory.path().join(format!("Snow Shot {name}"));
        let data = directory.path().join(name);
        if name.starts_with("external") {
            fs::create_dir(&data).unwrap();
            fs::write(data.join("config.json"), b"preserve custom configuration").unwrap();
        }
        install(&root, &data);

        uninstall(Path::new(env!("CARGO_BIN_EXE_snow-shot-updater")), &root);

        assert!(!root.join(edition::APP_PATH).exists());
        assert!(!root.join(edition::UPDATER_PATH).exists());
        if data.exists() {
            assert_eq!(
                fs::read(data.join("config.json")).unwrap(),
                b"preserve custom configuration"
            );
        }
    }
}

fn installer_command(operation: &str, root: &Path, backup: &Path) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_snow-shot-updater"))
        .args([operation, "--target"])
        .arg(root)
        .arg("--backup")
        .arg(backup)
        .output()
        .unwrap()
}

#[test]
fn installer_bootstrap_replaces_and_restores_the_legacy_helper() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("Snow Shot 安");
    install(&root, &directory.path().join("custom data 配置"));
    let target = root.join(edition::UPDATER_PATH);
    let bundled = fs::read(&target).unwrap();
    let mut original = bundled.clone();
    original.extend_from_slice(b"legacy helper overlay");
    fs::write(&target, &original).unwrap();
    let backup = directory.path().join("previous-updater.exe");

    assert!(
        installer_command("--prepare-installer-upgrade", &root, &backup)
            .status
            .success()
    );
    assert_eq!(fs::read(&target).unwrap(), bundled);
    assert_eq!(fs::read(&backup).unwrap(), original);
    assert!(root.join(edition::APP_PATH).is_file());

    assert!(
        installer_command("--restore-installer-upgrade", &root, &backup)
            .status
            .success()
    );
    assert_eq!(fs::read(&target).unwrap(), original);
    assert!(root.join(edition::APP_PATH).is_file());
}

#[test]
fn installer_bootstrap_preserves_the_helper_when_preparation_fails() {
    use fs2::FileExt;
    use std::{fs::OpenOptions, os::windows::fs::OpenOptionsExt};
    use windows::Win32::Storage::FileSystem::FILE_SHARE_READ;

    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("Snow Shot 安");
    install(&root, &directory.path().join("custom data 配置"));
    let target = root.join(edition::UPDATER_PATH);
    fs::write(&target, b"legacy helper").unwrap();
    let backup = directory.path().join("previous-updater.exe");
    fs::create_dir_all(root.join(edition::UPDATE_WORK)).unwrap();
    let lock = fs::File::create(root.join(edition::UPDATE_WORK).join("transaction.lock")).unwrap();
    lock.lock_exclusive().unwrap();
    let output = installer_command("--prepare-installer-upgrade", &root, &backup);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("update_lock_failed"));
    assert_eq!(fs::read(&target).unwrap(), b"legacy helper");
    assert!(!backup.exists());
    drop(lock);

    let held = OpenOptions::new()
        .read(true)
        .share_mode(FILE_SHARE_READ.0)
        .open(&target)
        .unwrap();
    assert!(
        !installer_command("--prepare-installer-upgrade", &root, &backup)
            .status
            .success()
    );
    assert_eq!(fs::read(&target).unwrap(), b"legacy helper");
    drop(held);

    assert!(
        !installer_command(
            "--prepare-installer-upgrade",
            &root,
            &root.join("backup.exe")
        )
        .status
        .success()
    );
    assert_eq!(fs::read(&target).unwrap(), b"legacy helper");
    assert!(root.join(edition::APP_PATH).is_file());
}

#[test]
fn installer_rollback_preserves_a_helper_changed_by_another_operation() {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("Snow Shot 安");
    install(&root, &directory.path().join("custom data 配置"));
    let backup = directory.path().join("previous-updater.exe");
    assert!(
        installer_command("--prepare-installer-upgrade", &root, &backup)
            .status
            .success()
    );
    let target = root.join(edition::UPDATER_PATH);
    fs::write(&target, b"a different committed helper").unwrap();

    assert!(
        installer_command("--restore-installer-upgrade", &root, &backup)
            .status
            .success()
    );
    assert_eq!(fs::read(&target).unwrap(), b"a different committed helper");
}

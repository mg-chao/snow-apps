//! Transaction behavior ported from the C++ update tests: apply/rollback
//! across variants, failure injection at journal checkpoints, adversarial
//! archives, reserved-path preservation, long roots, pruning, and real
//! process-termination recovery driven through the compiled helper.

use snow_updater::crypto::signer::TestSigner;
use snow_updater::installation::{prune_update_work, transaction_pending};
use snow_updater::testing::{Fixture, age_file_beyond_prune, put, read};
use snow_updater::transaction::{
    TransactionHooks, apply_transaction, recover_transaction, uninstall_owned_files,
};

fn hooks_always_ready() -> TransactionHooks {
    TransactionHooks {
        probe: Some(Box::new(|| true)),
        checkpoint: None,
    }
}

fn hooks_failing_at(point: &'static str) -> TransactionHooks {
    TransactionHooks {
        probe: Some(Box::new(move || point != "probe")),
        checkpoint: Some(Box::new(move |current| {
            if current == point {
                panic!("injected failure at {point}");
            }
        })),
    }
}

fn expect_failure<T>(result: snow_updater::errors::Result<T>) {
    assert!(result.is_err(), "operation must fail");
}

#[test]
fn applies_and_rolls_back_across_variants() {
    let signer = TestSigner::new();
    for variant in ["online", "offline", "portable"] {
        let fixture = Fixture::new(&signer, variant, false, false);
        apply_transaction(
            &fixture.root,
            &fixture.archive,
            &fixture.release,
            hooks_always_ready(),
        )
        .expect("apply transaction");
        assert_eq!(read(&fixture.root, "bin/snow_shot.exe"), "new executable");
        assert!(
            !fixture.root.join("bin/obsolete.txt").exists(),
            "obsolete owned file removed"
        );
        fixture.preserved();
        expect_failure(apply_transaction(
            &fixture.root,
            &fixture.archive,
            &fixture.release,
            TransactionHooks::default(),
        ));
    }
}

#[test]
fn injected_checkpoint_failures_roll_back_completely() {
    let signer = TestSigner::new();
    for point in [
        "prepared",
        "bin/snow_shot.exe",
        "bin/new.txt",
        "bin/obsolete.txt",
        "snow-shot-installation.json",
        "registry",
        "probe",
    ] {
        let fixture = Fixture::simple(&signer);
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            apply_transaction(
                &fixture.root,
                &fixture.archive,
                &fixture.release,
                hooks_failing_at(point),
            )
        }));
        let failed = result.is_err() || matches!(result, Ok(Err(_)));
        assert!(
            failed,
            "transaction must report injected failure at {point}"
        );
        assert_eq!(read(&fixture.root, "bin/snow_shot.exe"), "old executable");
        assert_eq!(read(&fixture.root, "bin/obsolete.txt"), "old data");
        assert!(
            !fixture.root.join("bin/new.txt").exists(),
            "new file removed after rollback"
        );
        assert!(
            !transaction_pending(&fixture.root),
            "recovery journal finished"
        );
        fixture.preserved();
    }
}

#[test]
fn rejects_unsafe_and_inconsistent_archives_before_mutation() {
    let signer = TestSigner::new();
    let unsafe_archive = Fixture::new(&signer, "portable", true, false);
    expect_failure(apply_transaction(
        &unsafe_archive.root,
        &unsafe_archive.archive,
        &unsafe_archive.release,
        TransactionHooks::default(),
    ));
    unsafe_archive.preserved();

    let inconsistent = Fixture::new(&signer, "portable", false, true);
    expect_failure(apply_transaction(
        &inconsistent.root,
        &inconsistent.archive,
        &inconsistent.release,
        TransactionHooks::default(),
    ));
    assert_eq!(
        read(&inconsistent.root, "bin/snow_shot.exe"),
        "old executable"
    );

    let corrupt = Fixture::simple(&signer);
    std::fs::write(&corrupt.archive, b"broken ZIP").expect("write corrupt archive");
    expect_failure(apply_transaction(
        &corrupt.root,
        &corrupt.archive,
        &corrupt.release,
        TransactionHooks::default(),
    ));
    corrupt.preserved();
}

#[test]
fn preserves_conflicting_user_files() {
    let signer = TestSigner::new();
    let conflict = Fixture::simple(&signer);
    put(&conflict.root, "bin/new.txt", b"user-owned file");
    expect_failure(apply_transaction(
        &conflict.root,
        &conflict.archive,
        &conflict.release,
        hooks_always_ready(),
    ));
    assert_eq!(read(&conflict.root, "bin/new.txt"), "user-owned file");
}

#[test]
fn rejects_data_directory_overlaps() {
    let signer = TestSigner::new();
    let custom_data = Fixture::simple(&signer);
    put(
        &custom_data.root,
        "bin/__data_directory",
        "\u{FEFF}new.txt".as_bytes(),
    );
    expect_failure(apply_transaction(
        &custom_data.root,
        &custom_data.archive,
        &custom_data.release,
        TransactionHooks::default(),
    ));

    let reserved = Fixture::simple(&signer);
    put(
        &reserved.root,
        "bin/__data_directory",
        b"../.snow-shot-update/stage",
    );
    put(
        &reserved.root,
        ".snow-shot-update/stage/user.json",
        b"precious reserved-path data",
    );
    expect_failure(apply_transaction(
        &reserved.root,
        &reserved.archive,
        &reserved.release,
        TransactionHooks::default(),
    ));
    expect_failure(uninstall_owned_files(&reserved.root));
    assert_eq!(
        read(&reserved.root, ".snow-shot-update/stage/user.json"),
        "precious reserved-path data"
    );
}

#[test]
fn long_paths_and_inventory_uninstall() {
    let signer = TestSigner::new();
    let mut fixture = Fixture::simple(&signer);
    let mut extended = fixture.temporary.clone();
    for _ in 0..8 {
        extended.push("long-installation-directory-segment");
    }
    std::fs::create_dir_all(extended.parent().expect("parent")).expect("create long parent");
    std::fs::rename(&fixture.root, &extended).expect("move fixture beyond MAX_PATH");
    fixture.root = extended;
    apply_transaction(
        &fixture.root,
        &fixture.archive,
        &fixture.release,
        hooks_always_ready(),
    )
    .expect("apply beyond MAX_PATH");
    fixture.preserved();
    uninstall_owned_files(&fixture.root).expect("uninstall owned files");
    fixture.preserved();
    assert!(
        !fixture.root.join("bin/snow_shot.exe").exists(),
        "inventory uninstall removes payload"
    );
}

#[test]
fn prune_removes_only_aged_generated_files() {
    let signer = TestSigner::new();
    let fixture = Fixture::simple(&signer);
    let stale = fixture
        .root
        .join(format!(".snow-shot-update/worker-{}.exe", "a".repeat(32)));
    put(&fixture.root, &stale.to_string_lossy(), b"old worker");
    put(
        &fixture.root,
        ".snow-shot-update/operator-notes.txt",
        b"preserve notes",
    );
    age_file_beyond_prune(&stale, 2);
    prune_update_work(&fixture.root).expect("prune work area");
    assert!(!stale.exists(), "aged generated worker removed");
    assert_eq!(
        read(&fixture.root, ".snow-shot-update/operator-notes.txt"),
        "preserve notes"
    );
}

#[test]
fn crash_at_journal_checkpoint_recovers_after_process_termination() {
    let helper = env!("CARGO_BIN_EXE_snow-shot-updater");
    let signer = TestSigner::new();
    for point in [
        "prepared",
        "bin/snow_shot.exe",
        "snow-shot-installation.json",
        "registry",
    ] {
        let fixture = Fixture::simple(&signer);
        let (manifest, keys) =
            snow_updater::testing::write_signed_manifests(&fixture.temporary, &fixture, &signer)
                .expect("write manifests");
        let status = std::process::Command::new(helper)
            .args(["--crash-apply", "--crash-root"])
            .arg(&fixture.root)
            .arg("--crash-archive")
            .arg(&fixture.archive)
            .arg("--crash-manifest")
            .arg(&manifest)
            .arg("--crash-keys")
            .arg(&keys)
            .arg("--crash-point")
            .arg(point)
            .status()
            .expect("spawn crashing helper");
        assert_eq!(
            status.code(),
            Some(77),
            "terminate updater at journal checkpoint"
        );
        assert!(
            transaction_pending(&fixture.root),
            "power loss leaves recoverable journal"
        );
        recover_transaction(&fixture.root).expect("recover after process termination");
        assert_eq!(read(&fixture.root, "bin/snow_shot.exe"), "old executable");
        assert!(
            !transaction_pending(&fixture.root),
            "interrupted transaction resolved"
        );
        fixture.preserved();
    }
}

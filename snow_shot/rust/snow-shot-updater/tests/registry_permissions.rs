#![cfg(windows)]

// A separate integration-test process keeps predefined-hive redirection away
// from parallel unit tests. All registry operations target disposable HKCU keys.
use snow_shot_updater::{contract::*, fsutil, platform, transaction::*};
use std::{fs, io::Write, path::Path};
use windows::Win32::Foundation::{ERROR_SUCCESS, HLOCAL, LocalFree};
use windows::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
use windows::Win32::Security::{DACL_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR};
use windows::Win32::System::Registry::*;
use windows::core::PCWSTR;

const INSTALL: &str = "Software\\Snow Apps\\SnowShot";
const UNINSTALL: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShot";

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

struct Key(HKEY);
impl Drop for Key {
    fn drop(&mut self) {
        unsafe { RegCloseKey(self.0).ok().unwrap() };
    }
}

fn create(hive: HKEY, path: &str) -> Key {
    let path = wide(path);
    let mut key = HKEY::default();
    unsafe {
        RegCreateKeyExW(
            hive,
            PCWSTR(path.as_ptr()),
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_ALL_ACCESS | KEY_WOW64_32KEY,
            None,
            &mut key,
            None,
        )
        .ok()
        .unwrap();
    }
    Key(key)
}

fn set(key: &Key, name: &str, value: &str) {
    let name = wide(name);
    let value = wide(value);
    unsafe {
        RegSetValueExW(
            key.0,
            PCWSTR(name.as_ptr()),
            None,
            REG_SZ,
            Some(std::slice::from_raw_parts(
                value.as_ptr().cast(),
                value.len() * 2,
            )),
        )
        .ok()
        .unwrap();
    }
}

fn acl(key: &Key, denied_rights: u32) {
    let sddl = wide(&format!("D:P(D;;0x{denied_rights:x};;;WD)(A;;KA;;;WD)"));
    let mut descriptor = PSECURITY_DESCRIPTOR::default();
    unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            PCWSTR(sddl.as_ptr()),
            1,
            &mut descriptor,
            None,
        )
        .unwrap();
        let status = RegSetKeySecurity(key.0, DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(Some(HLOCAL(descriptor.0)));
        status.ok().unwrap();
    }
}

struct RegistrySandbox {
    name: Vec<u16>,
    real_user: Key,
    _root: Key,
    _machine: Key,
    _user: Key,
}

impl RegistrySandbox {
    fn new() -> Self {
        let real_user = create(HKEY_CURRENT_USER, "Software");
        let name = wide(&format!("SnowShotUpdaterTest-{}", uuid::Uuid::new_v4()));
        let root = create(
            real_user.0,
            &String::from_utf16_lossy(&name[..name.len() - 1]),
        );
        let machine = create(root.0, "machine");
        let user = create(root.0, "user");
        unsafe {
            RegOverridePredefKey(HKEY_LOCAL_MACHINE, Some(machine.0))
                .ok()
                .unwrap();
            RegOverridePredefKey(HKEY_CURRENT_USER, Some(user.0))
                .ok()
                .unwrap();
        }
        Self {
            name,
            real_user,
            _root: root,
            _machine: machine,
            _user: user,
        }
    }
}

impl Drop for RegistrySandbox {
    fn drop(&mut self) {
        unsafe {
            RegOverridePredefKey(HKEY_LOCAL_MACHINE, None).ok().unwrap();
            RegOverridePredefKey(HKEY_CURRENT_USER, None).ok().unwrap();
            RegDeleteTreeW(self.real_user.0, PCWSTR(self.name.as_ptr()))
                .ok()
                .unwrap();
        }
    }
}

fn descriptor(path: &str, bytes: &[u8]) -> UpdateFile {
    use sha2::{Digest, Sha256};
    UpdateFile {
        path: path.into(),
        size: bytes.len() as u64,
        sha256: format!("{:x}", Sha256::digest(bytes)),
    }
}

fn prepare(root: &Path, archive: &Path) -> UpdateRelease {
    fs::create_dir_all(root.join("bin")).unwrap();
    let record = |version: &str, bytes: &[u8]| InstallationRecord {
        schema: 1,
        variant: "online".into(),
        version: version.into(),
        files: vec![descriptor("bin/snow_shot.exe", bytes)],
    };
    fs::write(root.join("bin/snow_shot.exe"), b"old").unwrap();
    fs::write(
        root.join(INSTALLATION_RECORD),
        serde_json::to_vec(&record("1.0.0", b"old")).unwrap(),
    )
    .unwrap();
    let next = serde_json::to_vec(&record("2.0.0", b"new")).unwrap();
    let entries = [
        ("bin/snow_shot.exe", b"new".as_slice()),
        (INSTALLATION_RECORD, &next),
    ];
    let mut zip = zip::ZipWriter::new(fs::File::create(archive).unwrap());
    for (name, bytes) in entries {
        zip.start_file(name, zip::write::SimpleFileOptions::default())
            .unwrap();
        zip.write_all(bytes).unwrap();
    }
    zip.finish().unwrap();
    UpdateRelease {
        version: "2.0.0".into(),
        envelope: vec![],
        packages: vec![UpdatePackage {
            variant: "online".into(),
            kind: "update".into(),
            path: "setup/snow-shot_windows-x64-online-update.zip".into(),
            size: fs::metadata(archive).unwrap().len(),
            sha256: fsutil::sha256_file(archive).unwrap(),
            files: entries
                .iter()
                .map(|(name, bytes)| descriptor(name, bytes))
                .collect(),
        }],
    }
}

#[test]
fn registry_permissions_and_transaction_recovery() {
    let _registry = RegistrySandbox::new();
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path().join("SnowShot");
    let archive = directory.path().join("update.zip");
    let release = prepare(&root, &archive);
    // The filesystem probe succeeds throughout these cases.
    tempfile::NamedTempFile::new_in(&root).unwrap();
    assert!(!platform::registered_version_requires_elevation(&root).unwrap());

    let registration = create(HKEY_LOCAL_MACHINE, INSTALL);
    set(&registration, "", root.to_str().unwrap());
    // Missing uninstall keys are checked without creating them.
    assert!(!platform::registered_version_requires_elevation(&root).unwrap());
    let mut absent = HKEY::default();
    assert_ne!(
        unsafe {
            RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                PCWSTR(wide(UNINSTALL).as_ptr()),
                None,
                KEY_READ | KEY_WOW64_32KEY,
                &mut absent,
            )
        },
        ERROR_SUCCESS
    );

    let uninstall_parent = create(
        HKEY_LOCAL_MACHINE,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    );
    acl(&uninstall_parent, KEY_CREATE_SUB_KEY.0);
    assert!(platform::registered_version_requires_elevation(&root).unwrap());
    acl(&uninstall_parent, 0);

    let uninstall = create(HKEY_LOCAL_MACHINE, UNINSTALL);
    set(&uninstall, "DisplayVersion", "1.0.0");
    assert!(!platform::registered_version_requires_elevation(&root).unwrap());
    acl(&uninstall, KEY_SET_VALUE.0);
    assert!(platform::registered_version_requires_elevation(&root).unwrap());
    platform::write_registered_version(&root, "1.0.0").unwrap();
    let error = platform::write_registered_version(&root, "2.0.0").unwrap_err();
    let detail = error.detail.unwrap();
    assert!(detail.contains("HKLM") && detail.contains("Win32 error 5"));

    // A denied update must restore the files and finalize rollback without
    // requiring another registry write, even if DisplayVersion was absent.
    for previous_value_exists in [true, false] {
        if !previous_value_exists {
            unsafe {
                RegDeleteValueW(uninstall.0, PCWSTR(wide("DisplayVersion").as_ptr()))
                    .ok()
                    .unwrap();
            }
        }
        let error = apply_transaction(
            &root,
            &archive,
            &release,
            TransactionHooks {
                probe: Some(&|| true),
                checkpoint: None,
            },
        )
        .unwrap_err();
        assert_eq!(error.code, "registered_version_update_failed");
        assert_eq!(fs::read(root.join("bin/snow_shot.exe")).unwrap(), b"old");
        assert!(!transaction_pending(&root));
    }

    // Old journals conservatively restore the registry, but an already-correct
    // value does not require permissions and must not strand the installation.
    set(&uninstall, "DisplayVersion", "1.0.0");
    let journal = root.join(UPDATE_WORK).join("journal.json");
    fs::write(&journal, br#"{"schema":1,"state":"applying","version":"2.0.0","previousVersion":"1.0.0","files":[]}"#).unwrap();
    recover_transaction(&root).unwrap();
    assert!(!transaction_pending(&root));

    // Crash after a successful registry write: retain the journal while denied,
    // then restore the previous value once access is available again.
    set(&uninstall, "DisplayVersion", "2.0.0");
    fs::write(&journal, br#"{"schema":1,"state":"applying","version":"2.0.0","previousVersion":"1.0.0","restoreRegistry":true,"files":[]}"#).unwrap();
    assert!(recover_transaction(&root).is_err());
    assert!(transaction_pending(&root));
    acl(&uninstall, 0);
    recover_transaction(&root).unwrap();
    assert!(!transaction_pending(&root));
    acl(&uninstall, KEY_SET_VALUE.0);
    platform::write_registered_version(&root, "1.0.0").unwrap();
    acl(&uninstall, 0);

    // A failed startup probe must roll back both the payload and the registry.
    let error = apply_transaction(
        &root,
        &archive,
        &release,
        TransactionHooks {
            probe: Some(&|| false),
            checkpoint: None,
        },
    )
    .unwrap_err();
    assert_eq!(error.code, "startup_probe_failed");
    assert_eq!(fs::read(root.join("bin/snow_shot.exe")).unwrap(), b"old");
    assert!(!transaction_pending(&root));
    acl(&uninstall, KEY_SET_VALUE.0);
    platform::write_registered_version(&root, "1.0.0").unwrap();
    acl(&uninstall, 0);

    // A successful transaction still updates the registry and commits.
    apply_transaction(
        &root,
        &archive,
        &release,
        TransactionHooks {
            probe: Some(&|| true),
            checkpoint: None,
        },
    )
    .unwrap();
    acl(&uninstall, KEY_SET_VALUE.0);
    platform::write_registered_version(&root, "2.0.0").unwrap();
    acl(&uninstall, 0);

    // The same permission contract applies to per-user registrations.
    set(
        &registration,
        "",
        directory.path().join("other").to_str().unwrap(),
    );
    let user_registration = create(HKEY_CURRENT_USER, INSTALL);
    set(&user_registration, "", root.to_str().unwrap());
    let user_uninstall = create(HKEY_CURRENT_USER, UNINSTALL);
    assert!(!platform::registered_version_requires_elevation(&root).unwrap());
    acl(&user_uninstall, KEY_SET_VALUE.0);
    assert!(platform::registered_version_requires_elevation(&root).unwrap());
    acl(&user_uninstall, 0);
    set(
        &user_registration,
        "",
        directory.path().join("other").to_str().unwrap(),
    );

    // Registry discovery errors must not silently turn an install into portable.
    acl(&registration, KEY_QUERY_VALUE.0);
    assert!(platform::registered_target_matches(&root).is_err());
    acl(&registration, 0);
}

//! Fixture builders for contract and transaction tests. Compiled only for
//! test builds and the `signing` feature; never shipped.

use crate::contract::{METADATA_LIMIT, UpdateRelease};
use crate::crypto::signer::TestSigner;
use crate::errors::Result;
use crate::platform::fsx;
use serde_json::{Value, json};
use std::io::Write;
use std::path::{Path, PathBuf};

pub fn sha256_hex(bytes: &[u8]) -> String {
    crate::crypto::hex(&crate::crypto::sha256(bytes))
}

pub fn descriptor(path: &str, bytes: &[u8]) -> Value {
    json!({ "path": path, "size": bytes.len(), "sha256": sha256_hex(bytes) })
}

pub fn put(root: &Path, name: &str, bytes: &[u8]) {
    let path = root.join(name);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).expect("create fixture path");
    }
    std::fs::write(&path, bytes).expect("write fixture file");
}

pub fn read(root: &Path, name: &str) -> String {
    String::from_utf8(fsx::read_limited(&root.join(name), METADATA_LIMIT).expect("read fixture"))
        .unwrap()
}

/// Writes a deflate ZIP with the given entries, mirroring the publisher's
/// archive shape.
pub fn write_zip(path: &Path, entries: &[(String, Vec<u8>)]) {
    let file = std::fs::File::create(path).expect("create fixture ZIP");
    let mut writer = zip::ZipWriter::new(std::io::BufWriter::new(file));
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated);
    for (name, bytes) in entries {
        writer
            .start_file(name.as_str(), options)
            .expect("begin ZIP entry");
        writer.write_all(bytes).expect("write ZIP entry");
    }
    writer.finish().expect("finish fixture ZIP");
}

/// A complete installation fixture with a signed newer release, following
/// the historical C++ test fixture exactly.
pub struct Fixture {
    pub root: PathBuf,
    pub archive: PathBuf,
    pub payload: Value,
    pub envelope: Vec<u8>,
    pub release: UpdateRelease,
    pub temporary: PathBuf,
}

impl Fixture {
    pub fn new(
        signer: &TestSigner,
        variant: &str,
        unsafe_names: bool,
        inconsistent_record: bool,
    ) -> Fixture {
        let temporary = std::env::temp_dir().join(format!(
            "snow-updater-test-{}",
            uuid::Uuid::new_v4().simple()
        ));
        std::fs::create_dir_all(&temporary).expect("temporary update fixture");
        let root = temporary.join("installed app 安");
        std::fs::create_dir_all(&root).expect("create install fixture");
        let old_files = vec![
            descriptor("bin/snow_shot.exe", b"old executable"),
            descriptor("bin/obsolete.txt", b"old data"),
        ];
        put(&root, "bin/snow_shot.exe", b"old executable");
        put(&root, "bin/obsolete.txt", b"old data");
        put(&root, "bin/portable/user.json", b"precious settings");
        put(&root, "bin/__data_directory", b"portable");
        put(&root, "user-file.txt", b"preserve me");
        let record = serde_json::to_vec(&json!({
            "schema": 1,
            "version": "1.0.0-beta",
            "variant": variant,
            "files": old_files,
        }))
        .expect("serialize record");
        put(&root, "snow-shot-installation.json", &record);
        let mut files: Vec<(String, Vec<u8>)> = vec![
            ("bin/snow_shot.exe".to_owned(), b"new executable".to_vec()),
            (
                "bin/snow-shot-updater.exe".to_owned(),
                b"new updater".to_vec(),
            ),
            ("bin/new.txt".to_owned(), b"new data".to_vec()),
        ];
        if variant == "portable" {
            files.push(("bin/__data_directory".to_owned(), b"portable".to_vec()));
        }
        let owned: Vec<Value> = files
            .iter()
            .filter(|(name, _)| !inconsistent_record || name != "bin/new.txt")
            .map(|(name, bytes)| descriptor(name, bytes))
            .collect();
        let record = serde_json::to_vec(&json!({
            "schema": 1,
            "version": "1.0.0-beta.1",
            "variant": variant,
            "files": owned,
        }))
        .expect("serialize staged record");
        files.push(("snow-shot-installation.json".to_owned(), record));
        let archive = temporary.join("update.zip");
        let entries: Vec<(String, Vec<u8>)> = files
            .iter()
            .map(|(name, bytes)| {
                let name = if unsafe_names && name == "bin/new.txt" {
                    "../escape".to_owned()
                } else {
                    name.clone()
                };
                (name, bytes.clone())
            })
            .collect();
        write_zip(&archive, &entries);
        let inventory: Vec<Value> = files
            .iter()
            .map(|(name, bytes)| descriptor(name, bytes))
            .collect();
        let archive_bytes = std::fs::read(&archive).expect("read fixture ZIP");
        let mut packages: Vec<Value> = Vec::new();
        for variant_name in ["online", "offline", "portable"] {
            let kinds: [&str; 2] = if variant_name == "portable" {
                ["portable", ""]
            } else {
                ["installer", "update"]
            };
            for kind in kinds {
                if kind.is_empty() {
                    continue;
                }
                let suffix = if kind == "installer" {
                    ".exe"
                } else if kind == "update" {
                    "-update.zip"
                } else {
                    ".zip"
                };
                let mut package = json!({
                    "variant": variant_name,
                    "kind": kind,
                    "path": format!("setup/snow-shot_windows-x64-{variant_name}{suffix}"),
                    "size": archive_bytes.len(),
                    "sha256": sha256_hex(&archive_bytes),
                });
                if kind != "installer" {
                    package["files"] = Value::Array(inventory.clone());
                }
                packages.push(package);
            }
        }
        let payload = json!({
            "schema": 1,
            "version": "1.0.0-beta.1",
            "publishedAt": "2026-09-10T00:00:00Z",
            "platform": "windows-x64",
            "packages": packages,
        });
        let envelope = signer.envelope(&payload);
        let release = crate::contract::verify_release(&envelope, Some(&signer.public_keys_json))
            .expect("verify fixture release");
        Fixture {
            root,
            archive,
            payload,
            envelope,
            release,
            temporary,
        }
    }

    pub fn simple(signer: &TestSigner) -> Fixture {
        Fixture::new(signer, "portable", false, false)
    }

    pub fn preserved(&self) {
        assert_eq!(
            read(&self.root, "bin/portable/user.json"),
            "precious settings"
        );
        assert_eq!(read(&self.root, "user-file.txt"), "preserve me");
        assert_eq!(read(&self.root, "bin/__data_directory"), "portable");
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.temporary);
    }
}

/// Ages a file beyond the prune threshold using the Win32 timestamp APIs.
pub fn age_file_beyond_prune(path: &Path, days: u64) {
    use std::os::windows::ffi::OsStrExt;
    use windows::Win32::Foundation::{CloseHandle, FILETIME};
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, FILE_GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, SetFileTime,
    };
    use windows::core::PCWSTR;
    let wide: Vec<u16> = path
        .as_os_str()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    unsafe {
        let handle = CreateFileW(
            PCWSTR(wide.as_ptr()),
            FILE_GENERIC_WRITE.0,
            FILE_SHARE_READ,
            None,
            OPEN_EXISTING,
            Default::default(),
            None,
        )
        .expect("open for aging");
        let stale = std::time::SystemTime::now()
            .checked_sub(std::time::Duration::from_secs(days * 86400))
            .expect("valid stale time");
        let stale = stale
            .duration_since(std::time::UNIX_EPOCH)
            .expect("post-epoch");
        // Windows FILETIME counts 100ns intervals since 1601-01-01.
        const UNIX_EPOCH_FILETIME: u64 = 116_444_736_000_000_000;
        let intervals = UNIX_EPOCH_FILETIME
            + stale.as_secs() * 10_000_000
            + u64::from(stale.subsec_nanos()) / 100;
        let stamp = FILETIME {
            dwLowDateTime: intervals as u32,
            dwHighDateTime: (intervals >> 32) as u32,
        };
        SetFileTime(handle, Some(&stamp), Some(&stamp), Some(&stamp)).expect("age fixture");
        let _ = CloseHandle(handle);
    }
}

pub fn write_signed_manifests(
    directory: &Path,
    fixture: &Fixture,
    signer: &TestSigner,
) -> Result<(PathBuf, PathBuf)> {
    let manifest = directory.join("release.json");
    let keys = directory.join("keys.json");
    fsx::write_atomic(&manifest, &fixture.envelope)?;
    fsx::write_atomic(&keys, signer.public_keys_json.as_bytes())?;
    Ok((manifest, keys))
}

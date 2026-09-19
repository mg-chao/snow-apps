use crate::error::{Result, UpdateError, require};
use crate::fsutil;
use base64::Engine;
use rsa::BigUint;
use rsa::RsaPublicKey;
use rsa::pss::Pss;
use semver::{BuildMetadata, Version};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::path::Path;
use time::OffsetDateTime;
use time::format_description::well_known::Rfc3339;

pub const MAX_METADATA_BYTES: usize = 8 * 1024 * 1024;
pub const MAX_PAYLOAD_BYTES: u64 = 4 * 1024 * 1024 * 1024;
pub const MAX_FILES: usize = 20_000;

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct UpdateFile {
    pub path: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct UpdatePackage {
    pub variant: String,
    pub kind: String,
    pub path: String,
    pub size: u64,
    pub sha256: String,
    #[serde(default)]
    pub files: Vec<UpdateFile>,
}

#[derive(Clone, Debug)]
pub struct UpdateRelease {
    pub version: String,
    pub packages: Vec<UpdatePackage>,
    pub envelope: Vec<u8>,
}

impl UpdateRelease {
    pub fn update_package(&self, variant: &str) -> Result<&UpdatePackage> {
        self.packages
            .iter()
            .find(|package| package.variant == variant && package.kind != "installer")
            .ok_or_else(|| {
                UpdateError::new(
                    "no_matching_package",
                    "No update package matches this installation",
                )
            })
    }
}

#[derive(Deserialize)]
struct TrustedKeys {
    keys: Vec<TrustedKey>,
}

#[derive(Deserialize)]
struct TrustedKey {
    id: String,
    modulus: String,
    exponent: String,
}

fn string<'a>(object: &'a serde_json::Map<String, Value>, name: &str) -> &'a str {
    object.get(name).and_then(Value::as_str).unwrap_or_default()
}

fn positive_size(value: Option<&Value>, allow_zero: bool) -> Result<u64> {
    let size = value.and_then(Value::as_u64).unwrap_or(u64::MAX);
    require(
        size <= MAX_PAYLOAD_BYTES && (allow_zero || size > 0),
        "invalid_update_file_size",
        "Invalid update file size",
    )?;
    Ok(size)
}

fn hash_value(value: Option<&Value>) -> Result<String> {
    let hash = value.and_then(Value::as_str).unwrap_or_default();
    require(
        hash.len() == 64
            && hash
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)),
        "invalid_update_checksum",
        "Invalid update checksum",
    )?;
    Ok(hash.to_owned())
}

fn decode_base64(value: Option<&Value>) -> Result<Vec<u8>> {
    let encoded = value.and_then(Value::as_str).unwrap_or_default();
    let decoded = base64::engine::general_purpose::STANDARD
        .decode(encoded)
        .map_err(|_| {
            UpdateError::new(
                "invalid_signature_encoding",
                "Invalid update signature encoding",
            )
        })?;
    require(
        !decoded.is_empty(),
        "invalid_signature_encoding",
        "Invalid update signature encoding",
    )?;
    Ok(decoded)
}

pub fn parse_version(value: &str) -> Result<Version> {
    require(
        value.encode_utf16().count() <= 128,
        "invalid_semantic_version",
        "Invalid semantic version",
    )?;
    Version::parse(value)
        .map_err(|_| UpdateError::new("invalid_semantic_version", "Invalid semantic version"))
}

pub fn compare_versions(first: &str, second: &str) -> Result<std::cmp::Ordering> {
    let mut first = parse_version(first)?;
    let mut second = parse_version(second)?;
    first.build = BuildMetadata::EMPTY;
    second.build = BuildMetadata::EMPTY;
    Ok(first.cmp(&second))
}

pub fn safe_relative_path(path: &str) -> bool {
    if path.is_empty()
        || path.encode_utf16().count() > 220
        || path.contains(['\\', ':'])
        || path.starts_with('/')
    {
        return false;
    }
    path.split('/').all(|part| {
        if part.is_empty() || matches!(part, "." | "..") || part.ends_with(['.', ' ']) {
            return false;
        }
        let stem = part.split('.').next().unwrap_or_default().to_uppercase();
        let reserved = matches!(
            stem.as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$" | "CLOCK$"
        ) || stem.strip_prefix("COM").is_some_and(reserved_port_number)
            || stem.strip_prefix("LPT").is_some_and(reserved_port_number);
        !reserved
            && part
                .chars()
                .all(|character| character >= ' ' && !"<>\"|?*".contains(character))
    })
}

fn reserved_port_number(value: &str) -> bool {
    matches!(
        value,
        "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
    )
}

pub fn parse_file_inventory(value: Option<&Value>) -> Result<Vec<UpdateFile>> {
    let array = value.and_then(Value::as_array).ok_or_else(|| {
        UpdateError::new("invalid_update_inventory", "Invalid update file inventory")
    })?;
    require(
        !array.is_empty() && array.len() <= MAX_FILES,
        "invalid_update_inventory",
        "Invalid update file inventory",
    )?;
    let mut files = Vec::with_capacity(array.len());
    let mut seen = HashSet::with_capacity(array.len());
    let mut total = 0_u64;
    for value in array {
        let object = value.as_object().ok_or_else(|| {
            UpdateError::new("unsafe_update_inventory", "Unsafe update file inventory")
        })?;
        let path = string(object, "path").to_owned();
        let size = positive_size(object.get("size"), true)?;
        let sha256 = hash_value(object.get("sha256"))?;
        let lower = path.to_lowercase();
        require(
            safe_relative_path(&path)
                && seen.insert(lower.clone())
                && (path.starts_with("bin/")
                    || path.starts_with("share/snow-shot/")
                    || path == "snow-shot-installation.json")
                && !lower.starts_with("bin/portable/"),
            "unsafe_update_inventory",
            "Unsafe update file inventory",
        )?;
        total = total.checked_add(size).ok_or_else(|| {
            UpdateError::new("update_payload_too_large", "Update payload is too large")
        })?;
        require(
            total <= MAX_PAYLOAD_BYTES,
            "update_payload_too_large",
            "Update payload is too large",
        )?;
        files.push(UpdateFile { path, size, sha256 });
    }
    for file in &files {
        let mut parent = file.path.to_lowercase();
        while let Some(index) = parent.rfind('/') {
            parent.truncate(index);
            require(
                !seen.contains(&parent),
                "conflicting_update_paths",
                "Conflicting update file paths",
            )?;
        }
    }
    Ok(files)
}

pub fn compiled_trusted_keys() -> &'static [u8] {
    include_bytes!(concat!(env!("OUT_DIR"), "/update-trusted-keys.json"))
}

pub fn verify_release(envelope: &[u8], trusted_keys: Option<&[u8]>) -> Result<UpdateRelease> {
    require(
        envelope.len() <= MAX_METADATA_BYTES,
        "metadata_too_large",
        "Update metadata is too large",
    )?;
    let outer: Value = serde_json::from_slice(envelope).map_err(|_| {
        UpdateError::new(
            "unsupported_signature_schema",
            "Unsupported update signature schema",
        )
    })?;
    let outer = outer.as_object().ok_or_else(|| {
        UpdateError::new(
            "unsupported_signature_schema",
            "Unsupported update signature schema",
        )
    })?;
    require(
        outer.get("schema").and_then(Value::as_u64) == Some(1),
        "unsupported_signature_schema",
        "Unsupported update signature schema",
    )?;
    let payload = decode_base64(outer.get("payload"))?;
    let signature = decode_base64(outer.get("signature"))?;
    let key_id = string(outer, "keyId");
    let trusted_keys = match trusted_keys {
        Some(keys) => keys,
        None => compiled_trusted_keys(),
    };
    let keys: TrustedKeys = serde_json::from_slice(trusted_keys).map_err(|_| {
        UpdateError::new("invalid_release_public_key", "Invalid release public key")
    })?;
    let key = keys
        .keys
        .iter()
        .find(|key| key.id == key_id)
        .ok_or_else(|| {
            UpdateError::new(
                "release_signature_invalid",
                "Release signature is invalid or its signing key is not trusted",
            )
        })?;
    let modulus = base64::engine::general_purpose::STANDARD
        .decode(&key.modulus)
        .map_err(|_| {
            UpdateError::new("invalid_release_public_key", "Invalid release public key")
        })?;
    let exponent = base64::engine::general_purpose::STANDARD
        .decode(&key.exponent)
        .map_err(|_| {
            UpdateError::new("invalid_release_public_key", "Invalid release public key")
        })?;
    require(
        modulus.len() == 384 && (1..=8).contains(&exponent.len()),
        "invalid_release_public_key",
        "Invalid release public key",
    )?;
    let public_key = RsaPublicKey::new(
        BigUint::from_bytes_be(&modulus),
        BigUint::from_bytes_be(&exponent),
    )
    .map_err(|_| UpdateError::new("invalid_release_public_key", "Invalid release public key"))?;
    let digest = Sha256::digest(&payload);
    public_key
        .verify(Pss::new_with_salt::<Sha256>(32), &digest, &signature)
        .map_err(|_| {
            UpdateError::new(
                "release_signature_invalid",
                "Release signature is invalid or its signing key is not trusted",
            )
        })?;

    let signed: Value = serde_json::from_slice(&payload).map_err(|_| {
        UpdateError::new("unsupported_update_release", "Unsupported update release")
    })?;
    let object = signed.as_object().ok_or_else(|| {
        UpdateError::new("unsupported_update_release", "Unsupported update release")
    })?;
    let published_at = string(object, "publishedAt");
    require(
        object.get("schema").and_then(Value::as_u64) == Some(1)
            && string(object, "platform") == "windows-x64"
            && OffsetDateTime::parse(published_at, &Rfc3339).is_ok(),
        "unsupported_update_release",
        "Unsupported update release",
    )?;
    let version = string(object, "version").to_owned();
    parse_version(&version)?;
    let packages = object
        .get("packages")
        .and_then(Value::as_array)
        .ok_or_else(|| {
            UpdateError::new(
                "incomplete_windows_release",
                "The release must contain all five Windows packages",
            )
        })?;
    require(
        packages.len() == 5,
        "incomplete_windows_release",
        "The release must contain all five Windows packages",
    )?;
    let mut parsed = Vec::with_capacity(5);
    let mut paths = HashSet::new();
    let mut identities = HashSet::new();
    for value in packages {
        let package = value.as_object().ok_or_else(|| {
            UpdateError::new("unknown_package_variant", "Unknown update package variant")
        })?;
        let variant = string(package, "variant").to_owned();
        let kind = string(package, "kind").to_owned();
        let path = string(package, "path").to_owned();
        let size = positive_size(package.get("size"), false)?;
        let sha256 = hash_value(package.get("sha256"))?;
        let portable = variant == "portable" && kind == "portable";
        require(
            portable
                || (matches!(variant.as_str(), "online" | "offline")
                    && matches!(kind.as_str(), "installer" | "update")),
            "unknown_package_variant",
            "Unknown update package variant",
        )?;
        let expected = if portable {
            format!("setup/snow-shot_windows-x64-{variant}.zip")
        } else if kind == "installer" {
            format!("setup/snow-shot_windows-x64-{variant}.exe")
        } else {
            format!("setup/snow-shot_windows-x64-{variant}-update.zip")
        };
        let identity = format!("{variant}/{kind}");
        require(
            path == expected && paths.insert(path.clone()) && identities.insert(identity),
            "unexpected_package_url",
            "Unexpected update package URL",
        )?;
        let files = if kind == "installer" {
            Vec::new()
        } else {
            parse_file_inventory(package.get("files"))?
        };
        if kind != "installer" {
            for required in [
                "bin/snow_shot.exe",
                "bin/snow-shot-updater.exe",
                "snow-shot-installation.json",
            ] {
                require(
                    files.iter().any(|file| file.path == required),
                    "incomplete_update_archive",
                    "Incomplete update archive",
                )?;
            }
        }
        parsed.push(UpdatePackage {
            variant,
            kind,
            path,
            size,
            sha256,
            files,
        });
    }
    Ok(UpdateRelease {
        version,
        packages: parsed,
        envelope: envelope.to_vec(),
    })
}

pub fn verify_release_file(path: &Path) -> Result<UpdateRelease> {
    verify_release(
        &fsutil::read_limited(path, MAX_METADATA_BYTES as u64)?,
        None,
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;
    use rand::rngs::OsRng;
    use rsa::RsaPrivateKey;
    use rsa::traits::PublicKeyParts;
    use serde_json::json;

    fn file(path: &str) -> Value {
        json!({"path": path, "size": 0, "sha256": "0".repeat(64)})
    }

    fn inventory() -> Vec<Value> {
        vec![
            file("bin/snow_shot.exe"),
            file("bin/snow-shot-updater.exe"),
            file("snow-shot-installation.json"),
        ]
    }

    fn valid_payload() -> Value {
        let files = inventory();
        json!({
            "schema": 1,
            "platform": "windows-x64",
            "publishedAt": "2026-09-19T00:00:00Z",
            "version": "2.0.0",
            "futureField": {"is": "ignored"},
            "packages": [
                {"variant":"online", "kind":"installer", "path":"setup/snow-shot_windows-x64-online.exe", "size":1, "sha256":"1".repeat(64)},
                {"variant":"online", "kind":"update", "path":"setup/snow-shot_windows-x64-online-update.zip", "size":1, "sha256":"2".repeat(64), "files":files, "future":true},
                {"variant":"offline", "kind":"installer", "path":"setup/snow-shot_windows-x64-offline.exe", "size":1, "sha256":"3".repeat(64)},
                {"variant":"offline", "kind":"update", "path":"setup/snow-shot_windows-x64-offline-update.zip", "size":1, "sha256":"4".repeat(64), "files":inventory()},
                {"variant":"portable", "kind":"portable", "path":"setup/snow-shot_windows-x64-portable.zip", "size":1, "sha256":"5".repeat(64), "files":inventory()}
            ]
        })
    }

    fn trusted_key(private: &RsaPrivateKey, exponent: Option<Vec<u8>>) -> Vec<u8> {
        let public = private.to_public_key();
        serde_json::to_vec(&json!({"keys":[{
            "id":"test",
            "modulus":base64::engine::general_purpose::STANDARD.encode(public.n().to_bytes_be()),
            "exponent":base64::engine::general_purpose::STANDARD.encode(exponent.unwrap_or_else(|| public.e().to_bytes_be()))
        }]}))
        .unwrap()
    }

    fn sign_payload(payload: &Value, private: &RsaPrivateKey, salt_length: usize) -> Vec<u8> {
        let payload = serde_json::to_vec(payload).unwrap();
        let signature = private
            .sign_with_rng(
                &mut OsRng,
                Pss::new_with_salt::<Sha256>(salt_length),
                &Sha256::digest(&payload),
            )
            .unwrap();
        serde_json::to_vec(&json!({
            "schema":1,
            "keyId":"test",
            "payload":base64::engine::general_purpose::STANDARD.encode(payload),
            "signature":base64::engine::general_purpose::STANDARD.encode(signature),
            "unknownEnvelopeField":"ignored"
        }))
        .unwrap()
    }

    #[test]
    fn semantic_versions_follow_precedence_and_ignore_build_metadata() {
        let ordered = [
            "1.0.0-alpha",
            "1.0.0-alpha.1",
            "1.0.0-alpha.beta",
            "1.0.0-beta",
            "1.0.0-beta.2",
            "1.0.0-beta.11",
            "1.0.0-rc.1",
            "1.0.0",
        ];
        for pair in ordered.windows(2) {
            assert_eq!(
                compare_versions(pair[0], pair[1]).unwrap(),
                std::cmp::Ordering::Less
            );
        }
        assert_eq!(
            compare_versions("1.0.0+build.1", "1.0.0+build.2").unwrap(),
            std::cmp::Ordering::Equal
        );
        for invalid in ["", "1", "01.0.0", "1.0.0-", "v1.0.0", &"1".repeat(129)] {
            assert!(parse_version(invalid).is_err(), "accepted {invalid}");
        }
    }

    #[test]
    fn path_validation_matches_windows_contract() {
        for path in [
            "",
            "/bin/a",
            "bin\\a",
            "bin/../a",
            "bin/CON.txt",
            "bin/COM¹",
            "bin/a. ",
            "bin/a:b",
        ] {
            assert!(!safe_relative_path(path), "accepted {path}");
        }
        assert!(safe_relative_path("share/snow-shot/licenses/license.txt"));
    }

    #[test]
    fn inventory_rejects_collisions_overlaps_and_limits() {
        let duplicate_case = json!([file("bin/Name.dll"), file("bin/name.dll")]);
        assert!(parse_file_inventory(Some(&duplicate_case)).is_err());

        let overlap = json!([file("bin/component"), file("bin/component/file.dll")]);
        assert!(parse_file_inventory(Some(&overlap)).is_err());

        let unsigned_root = json!([file("outside/file.dll")]);
        assert!(parse_file_inventory(Some(&unsigned_root)).is_err());

        let portable_data = json!([file("bin/portable/settings.json")]);
        assert!(parse_file_inventory(Some(&portable_data)).is_err());

        let too_large = json!([
            {"path":"bin/one", "size":MAX_PAYLOAD_BYTES, "sha256":"0".repeat(64)},
            {"path":"bin/two", "size":1, "sha256":"0".repeat(64)}
        ]);
        assert!(parse_file_inventory(Some(&too_large)).is_err());
    }

    #[test]
    fn rsa_pss_envelope_enforces_key_payload_and_exact_salt() {
        let private = RsaPrivateKey::new(&mut OsRng, 3072).unwrap();
        let trusted = trusted_key(&private, None);
        let payload = valid_payload();
        let envelope = sign_payload(&payload, &private, 32);
        let release = verify_release(&envelope, Some(&trusted)).unwrap();
        assert_eq!(release.version, "2.0.0");
        assert_eq!(release.packages.len(), 5);

        let wrong_salt = sign_payload(&payload, &private, 20);
        assert_eq!(
            verify_release(&wrong_salt, Some(&trusted))
                .unwrap_err()
                .code,
            "release_signature_invalid"
        );

        let wrong_key = trusted_key(&private, Some(vec![3]));
        assert_eq!(
            verify_release(&envelope, Some(&wrong_key))
                .unwrap_err()
                .code,
            "release_signature_invalid"
        );

        let mut changed: Value = serde_json::from_slice(&envelope).unwrap();
        changed["payload"] = Value::String(
            base64::engine::general_purpose::STANDARD.encode(b"changed signed payload"),
        );
        assert_eq!(
            verify_release(&serde_json::to_vec(&changed).unwrap(), Some(&trusted))
                .unwrap_err()
                .code,
            "release_signature_invalid"
        );

        let malformed = br#"{"schema":1,"keyId":"test","payload":"%%%","signature":"%%%"}"#;
        assert_eq!(
            verify_release(malformed, Some(&trusted)).unwrap_err().code,
            "invalid_signature_encoding"
        );

        let bad_keys = br#"{"keys":[{"id":"test","modulus":"%%%","exponent":"AQAB"}]}"#;
        assert_eq!(
            verify_release(&envelope, Some(bad_keys)).unwrap_err().code,
            "invalid_release_public_key"
        );
    }

    #[test]
    fn signed_release_rejects_schema_platform_and_package_mutations() {
        let private = RsaPrivateKey::new(&mut OsRng, 3072).unwrap();
        let trusted = trusted_key(&private, None);

        let mut payload = valid_payload();
        payload["platform"] = Value::String("macos-arm64".to_owned());
        let error =
            verify_release(&sign_payload(&payload, &private, 32), Some(&trusted)).unwrap_err();
        assert_eq!(error.code, "unsupported_update_release");

        let mut payload = valid_payload();
        payload["schema"] = Value::from(2);
        assert_eq!(
            verify_release(&sign_payload(&payload, &private, 32), Some(&trusted))
                .unwrap_err()
                .code,
            "unsupported_update_release"
        );

        let mut payload = valid_payload();
        payload["packages"].as_array_mut().unwrap().pop();
        assert_eq!(
            verify_release(&sign_payload(&payload, &private, 32), Some(&trusted))
                .unwrap_err()
                .code,
            "incomplete_windows_release"
        );

        let mut payload = valid_payload();
        payload["packages"][4]["variant"] = Value::String("online".to_owned());
        assert!(verify_release(&sign_payload(&payload, &private, 32), Some(&trusted)).is_err());
    }

    proptest! {
        #[test]
        fn accepted_paths_always_obey_component_invariants(path in ".{0,300}") {
            if safe_relative_path(&path) {
                prop_assert!(!path.starts_with('/'));
                prop_assert!(!path.contains(['\\', ':']));
                prop_assert!(path.encode_utf16().count() <= 220);
                for component in path.split('/') {
                    prop_assert!(!component.is_empty());
                    prop_assert!(!matches!(component, "." | ".."));
                    prop_assert!(!component.ends_with(['.', ' ']));
                }
            }
        }
    }
}

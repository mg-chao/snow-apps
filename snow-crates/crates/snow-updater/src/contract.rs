//! The signed release contract: envelope parsing, RSA-3072/PSS signature
//! verification, package and inventory validation. This is the single source
//! of truth shared by the helper process and the application through C FFI.

use crate::crypto::{sha256_file, verify_rsa_pss_sha256};
use crate::errors::{Error, Result, require};
use crate::keys::TRUSTED_KEYS_JSON;
use crate::semver::validate_version;
use base64::Engine;
use serde_json::Value;
use std::collections::HashSet;
use std::path::Path;

pub const METADATA_LIMIT: u64 = 8 * 1024 * 1024;
pub const MAX_INVENTORY_FILES: usize = 20000;
pub const MAX_PACKAGE_BYTES: u64 = 4 * 1024 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpdateFile {
    pub path: String,
    pub size: i64,
    pub sha256: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct UpdatePackage {
    pub variant: String,
    pub kind: String,
    pub path: String,
    pub size: i64,
    pub sha256: String,
    pub files: Vec<UpdateFile>,
}

#[derive(Clone, Debug)]
pub struct UpdateRelease {
    pub version: String,
    pub packages: Vec<UpdatePackage>,
    /// The original signed envelope bytes, preserved verbatim.
    pub envelope: Vec<u8>,
}

impl UpdateRelease {
    pub fn update_package(&self, variant: &str) -> Result<&UpdatePackage> {
        self.packages
            .iter()
            .find(|package| package.variant == variant && package.kind != "installer")
            .ok_or_else(|| Error::fixed("No update package matches this installation"))
    }
}

pub fn decode_base64(text: &str) -> Result<Vec<u8>> {
    let decoded = base64::engine::general_purpose::STANDARD
        .decode(text.as_bytes())
        .map_err(|_| Error::fixed("Invalid update signature encoding"))?;
    require(!decoded.is_empty(), "Invalid update signature encoding")?;
    Ok(decoded)
}

/// JSON number to a size in [0, 4 GiB] with the historical double-exactness
/// rule: fractional values are rejected because they lose their integer form.
fn positive_size(value: &Value, allow_zero: bool) -> Result<i64> {
    let number = value
        .as_f64()
        .ok_or_else(|| Error::fixed("Invalid update file size"))?;
    let lower = if allow_zero { 0.0 } else { 1.0 };
    require(
        number >= lower && number <= MAX_PACKAGE_BYTES as f64 && number == (number as i64) as f64,
        "Invalid update file size",
    )?;
    Ok(number as i64)
}

fn hash_value(value: &Value) -> Result<String> {
    let hash = value.as_str().unwrap_or_default();
    require(
        hash.len() == 64
            && hash
                .chars()
                .all(|c| c.is_ascii_digit() || (c.is_ascii_lowercase() && c.is_ascii_alphabetic())),
        "Invalid update checksum",
    )?;
    Ok(hash.to_owned())
}

/// Windows-safe relative inventory path (forward separators only).
pub fn safe_relative_path(path: &str) -> bool {
    if path.is_empty()
        || path.len() > 220
        || path.contains('\\')
        || path.contains(':')
        || path.starts_with('/')
    {
        return false;
    }
    for part in path.split('/') {
        if part.is_empty()
            || part == "."
            || part == ".."
            || part.ends_with('.')
            || part.ends_with(' ')
            || is_reserved_component(part)
        {
            return false;
        }
        if part
            .chars()
            .any(|c| (c as u32) < 32 || "<>\"|?*".contains(c))
        {
            return false;
        }
    }
    true
}

/// Windows device names, optionally followed by one extension, matching the
/// historical reserved-name pattern (case-insensitive).
fn is_reserved_component(part: &str) -> bool {
    let stem = part.split_once('.').map_or(part, |(before, _)| before);
    let upper = stem.to_uppercase();
    matches!(
        upper.as_str(),
        "CON" | "PRN" | "AUX" | "NUL" | "CONIN$" | "CONOUT$" | "CLOCK$"
    ) || {
        let chars: Vec<char> = upper.chars().collect();
        (upper.starts_with("COM") || upper.starts_with("LPT"))
            && chars.len() == 4
            && (chars[3].is_ascii_digit() || matches!(chars[3], '¹' | '²' | '³'))
    }
}

/// Validates the exhaustive file inventory of an update ZIP.
pub fn parse_file_inventory(array: &[Value]) -> Result<Vec<UpdateFile>> {
    require(
        !array.is_empty() && array.len() <= MAX_INVENTORY_FILES,
        "Invalid update file inventory",
    )?;
    let mut files = Vec::with_capacity(array.len());
    let mut seen: HashSet<String> = HashSet::new();
    let mut total: i128 = 0;
    for value in array {
        let object = value
            .as_object()
            .ok_or_else(|| Error::fixed("Unsafe update file inventory"))?;
        let file = UpdateFile {
            path: object
                .get("path")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            size: positive_size(value_property(object, "size"), true)?,
            sha256: hash_value(value_property(object, "sha256"))?,
        };
        let lower = file.path.to_lowercase();
        require(
            safe_relative_path(&file.path)
                && !seen.contains(&lower)
                && (file.path.starts_with("bin/")
                    || file.path.starts_with("share/snow-shot/")
                    || file.path == "snow-shot-installation.json")
                && !lower.starts_with("bin/portable/"),
            "Unsafe update file inventory",
        )?;
        seen.insert(lower);
        total += file.size as i128;
        require(
            total <= MAX_PACKAGE_BYTES as i128,
            "Update payload is too large",
        )?;
        files.push(file);
    }
    for file in &files {
        let mut parent = file.path.to_lowercase();
        while let Some(index) = parent.rfind('/') {
            parent.truncate(index);
            require(!seen.contains(&parent), "Conflicting update file paths")?;
        }
    }
    Ok(files)
}

fn value_property<'a>(object: &'a serde_json::Map<String, Value>, key: &str) -> &'a Value {
    object.get(key).unwrap_or(&Value::Null)
}

/// Parses one ISO-8601 date or date-time accepted by `QDateTime` with
/// `Qt::ISODate`, validating calendar ranges.
fn is_valid_iso8601(text: &str) -> bool {
    fn digits(slice: &str) -> bool {
        !slice.is_empty() && slice.chars().all(|c| c.is_ascii_digit())
    }
    fn valid_date(year: &str, month: &str, day: &str) -> bool {
        let month: u32 = match month.parse() {
            Ok(value) => value,
            Err(_) => return false,
        };
        let day: u32 = match day.parse() {
            Ok(value) => value,
            Err(_) => return false,
        };
        if !(1..=12).contains(&month) || day == 0 {
            return false;
        }
        let year: i32 = year.parse().unwrap_or(0);
        let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        let limit = match month {
            1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
            4 | 6 | 9 | 11 => 30,
            2 => {
                if leap {
                    29
                } else {
                    28
                }
            }
            _ => return false,
        };
        day <= limit
    }
    let (date, rest) = match text.split_once(['T', ' ']) {
        Some((date, rest)) => (date, Some(rest)),
        None => (text, None),
    };
    let parts: Vec<&str> = date.split('-').collect();
    if parts.len() != 3 || !parts.iter().all(|p| digits(p)) {
        return false;
    }
    if !valid_date(parts[0], parts[1], parts[2]) {
        return false;
    }
    let Some(rest) = rest else { return true };
    let (time, offset) = match rest.find(['Z', '+']) {
        Some(index) if rest[index..].starts_with('Z') => (&rest[..index], Some("Z")),
        Some(index) => (&rest[..index], Some(&rest[index..])),
        None => match rest.rfind('-') {
            // A '-' can only be an offset separator inside the time segment.
            Some(index) if index >= 8 && rest[index..].len() >= 3 => {
                (&rest[..index], Some(&rest[index..]))
            }
            _ => (rest, None),
        },
    };
    let time_parts: Vec<&str> = time.split(':').collect();
    if !(2..=3).contains(&time_parts.len()) {
        return false;
    }
    if !digits(time_parts[0]) || !digits(time_parts[1]) {
        return false;
    }
    let hour: u32 = time_parts[0].parse().unwrap_or(99);
    let minute: u32 = time_parts[1].parse().unwrap_or(99);
    if hour > 23 || minute > 59 {
        return false;
    }
    if let Some(seconds) = time_parts.get(2) {
        let seconds = *seconds;
        let (whole, fraction) = match seconds.split_once('.') {
            Some((whole, fraction)) => (whole, Some(fraction)),
            None => (seconds, None),
        };
        if !digits(whole) || whole.parse::<u32>().unwrap_or(99) > 59 {
            return false;
        }
        if fraction.is_some_and(|fraction| !digits(fraction)) {
            return false;
        }
    }
    match offset {
        None | Some("Z") => true,
        Some(offset) => {
            let value = &offset[1..];
            let colon = value.len() == 5 && value.contains(':');
            let compact = value.len() == 4 && !value.contains(':');
            if !(colon || compact) || !digits(&value.replace(':', "")) {
                return false;
            }
            let (hh, mm) = match value.split_once(':') {
                Some((hh, mm)) => (hh, mm),
                None => (&value[..2], &value[2..]),
            };
            hh.parse::<u32>().unwrap_or(99) <= 23 && mm.parse::<u32>().unwrap_or(99) <= 59
        }
    }
}

fn verify_signature_with_keys(
    payload: &[u8],
    signature: &[u8],
    keys: &Value,
    key_id: &str,
) -> Result<bool> {
    let keys = keys
        .get("keys")
        .and_then(Value::as_array)
        .ok_or_else(|| Error::fixed("Invalid release public key"))?;
    for key in keys {
        let id = key.get("id").and_then(Value::as_str).unwrap_or_default();
        if id == key_id {
            let modulus = decode_base64(
                key.get("modulus")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?;
            let exponent = decode_base64(
                key.get("exponent")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?;
            return verify_rsa_pss_sha256(payload, signature, &modulus, &exponent);
        }
    }
    Ok(false)
}

/// Verifies a signed release envelope. `trusted_keys_json` overrides the
/// compiled-in trust store (used by tests); empty means compiled keys.
pub fn verify_release(envelope: &[u8], trusted_keys_json: Option<&str>) -> Result<UpdateRelease> {
    require(
        envelope.len() as u64 <= METADATA_LIMIT,
        "Update metadata is too large",
    )?;
    let outer: Value = serde_json::from_slice(envelope)
        .map_err(|_| Error::fixed("Unsupported update signature schema"))?;
    require(
        outer.get("schema").and_then(Value::as_i64) == Some(1),
        "Unsupported update signature schema",
    )?;
    let payload = decode_base64(
        outer
            .get("payload")
            .and_then(Value::as_str)
            .unwrap_or_default(),
    )?;
    let signature = decode_base64(
        outer
            .get("signature")
            .and_then(Value::as_str)
            .unwrap_or_default(),
    )?;
    let key_id = outer
        .get("keyId")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    let keys: Value = match trusted_keys_json {
        Some(text) if !text.is_empty() => {
            serde_json::from_str(text).map_err(|_| Error::fixed("Invalid release public key"))?
        }
        _ => serde_json::from_str(TRUSTED_KEYS_JSON)
            .map_err(|_| Error::fixed("Invalid release public key"))?,
    };
    let verified = verify_signature_with_keys(&payload, &signature, &keys, &key_id)?;
    require(
        verified,
        "Release signature is invalid or its signing key is not trusted",
    )?;
    let object: Value =
        serde_json::from_slice(&payload).map_err(|_| Error::fixed("Unsupported update release"))?;
    require(
        object.get("schema").and_then(Value::as_i64) == Some(1)
            && object.get("platform").and_then(Value::as_str) == Some("windows-x64")
            && is_valid_iso8601(
                object
                    .get("publishedAt")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            ),
        "Unsupported update release",
    )?;
    let mut release = UpdateRelease {
        version: object
            .get("version")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned(),
        packages: Vec::new(),
        envelope: envelope.to_vec(),
    };
    validate_version(&release.version)?;
    let mut paths: HashSet<String> = HashSet::new();
    let mut identities: HashSet<String> = HashSet::new();
    let packages = object
        .get("packages")
        .and_then(Value::as_array)
        .ok_or_else(|| Error::fixed("The release must contain all five Windows packages"))?;
    require(
        packages.len() == 5,
        "The release must contain all five Windows packages",
    )?;
    for value in packages {
        let package = value
            .as_object()
            .ok_or_else(|| Error::fixed("Unknown update package variant"))?;
        let mut parsed = UpdatePackage {
            variant: package
                .get("variant")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            kind: package
                .get("kind")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            path: package
                .get("path")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_owned(),
            size: positive_size(value_property(package, "size"), false)?,
            sha256: hash_value(value_property(package, "sha256"))?,
            files: Vec::new(),
        };
        let portable = parsed.variant == "portable" && parsed.kind == "portable";
        require(
            portable
                || ((parsed.variant == "online" || parsed.variant == "offline")
                    && (parsed.kind == "installer" || parsed.kind == "update")),
            "Unknown update package variant",
        )?;
        let expected = format!(
            "setup/snow-shot_windows-x64-{}{}",
            parsed.variant,
            if portable {
                ".zip"
            } else if parsed.kind == "installer" {
                ".exe"
            } else {
                "-update.zip"
            }
        );
        let identity = format!("{}/{}", parsed.variant, parsed.kind);
        require(
            parsed.path == expected
                && !paths.contains(&parsed.path)
                && !identities.contains(&identity),
            "Unexpected update package URL",
        )?;
        paths.insert(parsed.path.clone());
        identities.insert(identity);
        if parsed.kind != "installer" {
            let files = package
                .get("files")
                .and_then(Value::as_array)
                .ok_or_else(|| Error::fixed("Invalid update file inventory"))?;
            parsed.files = parse_file_inventory(files)?;
            for required in [
                "bin/snow_shot.exe",
                "bin/snow-shot-updater.exe",
                "snow-shot-installation.json",
            ] {
                require(
                    parsed.files.iter().any(|file| file.path == required),
                    "Incomplete update archive",
                )?;
            }
        }
        release.packages.push(parsed);
    }
    Ok(release)
}

/// Size plus SHA-256 check of a downloaded payload against the signed values.
pub fn verify_file(path: &Path, size: i64, sha256: &str) -> Result<()> {
    let actual_size = std::fs::metadata(path)
        .map_err(|_| {
            Error::fixed("Update payload size or checksum does not match the signed release")
        })?
        .len();
    let matches = actual_size == size as u64
        && sha256_file(path)
            .map(|hash| hash == sha256)
            .unwrap_or(false);
    require(
        matches,
        "Update payload size or checksum does not match the signed release",
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsafe_paths_are_rejected() {
        for path in [
            "../a",
            "bin/../../a",
            "C:/a",
            "bin/a:b",
            "bin/a.",
            "bin/CON.txt",
            "bin//a",
            "bin\\a",
            "",
            "bin/COM1",
            "bin/nul.exe.dat",
        ] {
            assert!(!safe_relative_path(path), "{path} must be rejected");
        }
        assert!(safe_relative_path("share/snow-shot/licenses/license.txt"));
        assert!(safe_relative_path("bin/snow_shot.exe"));
    }

    #[test]
    fn iso8601_matches_qt_acceptance() {
        assert!(is_valid_iso8601("2026-09-10T00:00:00Z"));
        assert!(is_valid_iso8601("2026-09-10 00:00:00"));
        assert!(is_valid_iso8601("2026-09-10"));
        assert!(is_valid_iso8601("2026-12-31T23:59:59.123+05:30"));
        assert!(is_valid_iso8601("2024-02-29T10:00:00Z"));
        assert!(!is_valid_iso8601("2025-02-29T10:00:00Z"));
        assert!(!is_valid_iso8601("2026-13-01"));
        assert!(!is_valid_iso8601("not-a-date"));
        assert!(!is_valid_iso8601("2026-09-10T25:00:00Z"));
        assert!(!is_valid_iso8601(""));
    }

    #[test]
    fn inventory_conflicts_are_rejected() {
        let json: Value = serde_json::from_str(
            r#"[{"path":"bin/a","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"},
                {"path":"bin/a/b","size":1,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]"#,
        )
        .unwrap();
        let array = json.as_array().unwrap();
        assert!(parse_file_inventory(array).is_err());
    }
}

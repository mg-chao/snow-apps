//! C ABI surface for the Snow Shot update contract. The application's
//! `UpdateService` and startup recovery consume these entry points so the
//! helper binary and the in-app verifier share one implementation.

#![allow(clippy::missing_safety_doc)]

use snow_updater::contract::{UpdateRelease, verify_file, verify_release};
use snow_updater::errors::{Error, message_count, message_for_code};
use snow_updater::installation::{installation_record, installation_root, transaction_pending};
use snow_updater::semver::compare_versions;

const VARIANT_CAPACITY: usize = 24;
const KIND_CAPACITY: usize = 16;
const PATH_CAPACITY: usize = 64;
const HASH_CAPACITY: usize = 65;
const VERSION_CAPACITY: usize = 129;
pub const SNOW_UPDATER_PACKAGE_COUNT: usize = 5;

/// One of the five fixed packages of a signed release.
#[repr(C)]
pub struct SnowUpdaterPackage {
    pub variant: [u16; VARIANT_CAPACITY],
    pub kind: [u16; KIND_CAPACITY],
    pub path: [u16; PATH_CAPACITY],
    pub size: i64,
    pub sha256: [u16; HASH_CAPACITY],
}

#[repr(C)]
pub struct SnowUpdaterRelease {
    pub version: [u16; VERSION_CAPACITY],
    pub packages: [SnowUpdaterPackage; SNOW_UPDATER_PACKAGE_COUNT],
}

#[repr(C)]
pub struct SnowUpdaterInstallation {
    pub variant: [u16; VARIANT_CAPACITY],
    pub version: [u16; VERSION_CAPACITY],
}

fn write_wide(target: &mut [u16], text: &str) {
    for (slot, unit) in target.iter_mut().zip(text.encode_utf16()) {
        *slot = unit;
    }
}

fn error_text(error: &Error, err_utf8: *mut u8, err_capacity: usize) -> i32 {
    let text = error.message().as_bytes();
    if !err_utf8.is_null() && err_capacity > 0 {
        let length = text.len().min(err_capacity - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), err_utf8, length);
            *err_utf8.add(length) = 0;
        }
    }
    -1
}

fn decode_input(text: *const u16, length: usize) -> String {
    if text.is_null() || length == 0 {
        return String::new();
    }
    let slice = unsafe { std::slice::from_raw_parts(text, length) };
    String::from_utf16_lossy(slice)
}

/// Verifies a signed release envelope. `trusted_keys_json` (optional) points
/// to a UTF-16 trust store overriding the compiled-in keys.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_verify_release(
    envelope: *const u16,
    envelope_length: usize,
    trusted_keys_json: *const u16,
    trusted_keys_length: usize,
    out: *mut SnowUpdaterRelease,
    err_utf8: *mut u8,
    err_capacity: usize,
) -> i32 {
    let bytes: Vec<u16> = if envelope.is_null() || envelope_length == 0 {
        Vec::new()
    } else {
        unsafe { std::slice::from_raw_parts(envelope, envelope_length) }.to_vec()
    };
    let bytes = String::from_utf16_lossy(&bytes).into_bytes();
    let keys = if trusted_keys_json.is_null() || trusted_keys_length == 0 {
        None
    } else {
        let slice = unsafe { std::slice::from_raw_parts(trusted_keys_json, trusted_keys_length) };
        Some(String::from_utf16_lossy(slice))
    };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        verify_release(&bytes, keys.as_deref())
    })) {
        Ok(Ok(release)) => {
            if out.is_null() {
                return -2;
            }
            fill_release(unsafe { &mut *out }, &release);
            0
        }
        Ok(Err(error)) => error_text(&error, err_utf8, err_capacity),
        Err(_) => error_text(
            &Error::raw("Update contract verifier panicked"),
            err_utf8,
            err_capacity,
        ),
    }
}

fn fill_release(out: &mut SnowUpdaterRelease, release: &UpdateRelease) {
    write_wide(&mut out.version, &release.version);
    for (slot, package) in out.packages.iter_mut().zip(&release.packages) {
        write_wide(&mut slot.variant, &package.variant);
        write_wide(&mut slot.kind, &package.kind);
        write_wide(&mut slot.path, &package.path);
        slot.size = package.size;
        write_wide(&mut slot.sha256, &package.sha256);
    }
}

/// Compares two versions; the ordering lands in `result` (-1, 0, 1).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_compare_versions(
    first: *const u16,
    first_length: usize,
    second: *const u16,
    second_length: usize,
    result: *mut i32,
    err_utf8: *mut u8,
    err_capacity: usize,
) -> i32 {
    match compare_versions(
        &decode_input(first, first_length),
        &decode_input(second, second_length),
    ) {
        Ok(ordering) => {
            if !result.is_null() {
                unsafe {
                    *result = match ordering {
                        std::cmp::Ordering::Less => -1,
                        std::cmp::Ordering::Equal => 0,
                        std::cmp::Ordering::Greater => 1,
                    };
                }
            }
            0
        }
        Err(error) => error_text(&error, err_utf8, err_capacity),
    }
}

/// Size plus SHA-256 check of a downloaded payload.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_verify_file(
    path: *const u16,
    path_length: usize,
    expected_size: i64,
    sha256_hex: *const u16,
    sha256_length: usize,
    err_utf8: *mut u8,
    err_capacity: usize,
) -> i32 {
    let path = decode_input(path, path_length);
    let hash = decode_input(sha256_hex, sha256_length);
    match verify_file(std::path::Path::new(&path), expected_size, &hash) {
        Ok(()) => 0,
        Err(error) => error_text(&error, err_utf8, err_capacity),
    }
}

/// Reads and validates the installation record of a root.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_installation_record(
    root: *const u16,
    root_length: usize,
    out: *mut SnowUpdaterInstallation,
    err_utf8: *mut u8,
    err_capacity: usize,
) -> i32 {
    let root = decode_input(root, root_length);
    match installation_record(std::path::Path::new(&root)) {
        Ok(record) => {
            if !out.is_null() {
                let target = unsafe { &mut *out };
                write_wide(&mut target.variant, &record.variant);
                write_wide(&mut target.version, &record.version);
            }
            0
        }
        Err(error) => error_text(&error, err_utf8, err_capacity),
    }
}

/// Computes the installation root from the executable directory; writes the
/// UTF-16 result and returns its length, or a negative error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_installation_root(
    executable_directory: *const u16,
    executable_directory_length: usize,
    out: *mut u16,
    out_capacity: usize,
) -> i32 {
    let directory = decode_input(executable_directory, executable_directory_length);
    let root = installation_root(std::path::Path::new(&directory));
    let text = root.to_string_lossy();
    let units: Vec<u16> = text.encode_utf16().collect();
    if units.len() >= out_capacity {
        return -3;
    }
    if !out.is_null() {
        for (slot, unit) in unsafe { std::slice::from_raw_parts_mut(out, out_capacity) }
            .iter_mut()
            .zip(&units)
        {
            *slot = *unit;
        }
        unsafe { *out.add(units.len()) = 0 };
    }
    units.len() as i32
}

/// Whether a recovery journal is pending for the installation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_transaction_pending(
    root: *const u16,
    root_length: usize,
) -> i32 {
    let root = decode_input(root, root_length);
    i32::from(!transaction_pending(std::path::Path::new(&root)))
}

/// The catalog diagnostic for a C FFI error code (1-based); null when the
/// code is unknown.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_error_message(code: u32) -> *const u8 {
    match message_for_code(code) {
        Some(text) => text.as_ptr(),
        None => std::ptr::null(),
    }
}

/// Number of valid error codes, for catalog consistency checks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_updater_error_count() -> u32 {
    message_count()
}

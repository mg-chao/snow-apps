//! Installation registry access: the `Software\Snow Apps\SnowShot` location
//! used to authorize elevation, the uninstall `DisplayVersion`, and the
//! per-user auto-start Run values reconciled during migration/uninstall.

use super::{hresult_from_win32, wide_str, windows_error};
use crate::errors::{Error, Result, require};
use crate::paths::{clean_path, paths_ci_eq};
use std::path::Path;
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS, KEY_QUERY_VALUE, KEY_READ,
    KEY_SET_VALUE, KEY_WOW64_32KEY, REG_OPTION_NON_VOLATILE, REG_SZ, RRF_RT_REG_SZ,
    RRF_SUBKEY_WOW6432KEY, RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegEnumKeyExW,
    RegGetValueW, RegOpenKeyExW, RegSetValueExW,
};
use windows::core::PCWSTR;

const INSTALL_KEY: &str = "Software\\Snow Apps\\SnowShot";
const UNINSTALL_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShot";
const RUN_SUBKEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

fn read_reg_sz(hive: HKEY, subkey: &str, value: Option<&str>, wow64: bool) -> Option<String> {
    let mut buffer = [0u16; 32768];
    let mut size = buffer.len() as u32;
    let flags = RRF_RT_REG_SZ
        | if wow64 {
            RRF_SUBKEY_WOW6432KEY
        } else {
            Default::default()
        };
    let value = match value {
        Some(text) => PCWSTR(wide_str(text).as_ptr()),
        None => PCWSTR::null(),
    };
    let status = unsafe {
        RegGetValueW(
            hive,
            PCWSTR(wide_str(subkey).as_ptr()),
            value,
            flags,
            None,
            Some(buffer.as_mut_ptr().cast()),
            Some(&mut size),
        )
    };
    if status.0 != 0 || !size.is_multiple_of(2) {
        return None;
    }
    let length = (size / 2) as usize;
    let text = String::from_utf16_lossy(&buffer[..length]);
    Some(text.trim_end_matches('\0').to_owned())
}

/// The registered install root must match under HKLM or HKCU (32-bit view).
pub fn registered_installation_matches(root: &Path) -> bool {
    let root = clean_path(&root.to_string_lossy());
    [HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER]
        .into_iter()
        .any(|hive| {
            read_reg_sz(hive, INSTALL_KEY, None, true)
                .is_some_and(|location| paths_ci_eq(&clean_path(&location), &root))
        })
}

pub fn validate_registered_target(root: &Path) -> Result<()> {
    require(
        registered_installation_matches(root),
        "Elevation requires a registered Snow Shot installation",
    )
}

/// Updates `DisplayVersion` on the matching uninstall registration.
pub fn write_registered_version(root: &Path, version: &str) -> Result<()> {
    for hive in [HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER] {
        let Some(location) = read_reg_sz(hive, INSTALL_KEY, None, true) else {
            continue;
        };
        if !paths_ci_eq(&clean_path(&location), &clean_path(&root.to_string_lossy())) {
            continue;
        }
        unsafe {
            let mut key = HKEY::default();
            let created = RegCreateKeyExW(
                hive,
                PCWSTR(wide_str(UNINSTALL_KEY).as_ptr()),
                None,
                None,
                REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE | KEY_WOW64_32KEY,
                None,
                &mut key,
                None,
            );
            if created.0 != 0 {
                return Err(windows_error(hresult_from_win32(created.0)));
            }
            let mut data = wide_str(version);
            data.pop(); // length includes the terminator
            let written = RegSetValueExW(
                key,
                PCWSTR(wide_str("DisplayVersion").as_ptr()),
                None,
                REG_SZ,
                Some(std::slice::from_raw_parts(
                    data.as_ptr().cast::<u8>(),
                    (data.len() + 1) * std::mem::size_of::<u16>(),
                )),
            );
            let _ = RegCloseKey(key);
            if written.0 != 0 {
                return Err(Error::fixed(
                    "Could not update the registered application version",
                ));
            }
            return Ok(());
        }
    }
    Ok(())
}

/// Result of reconciling one Run value.
pub enum Reconciled {
    /// No verifiable Snow Shot registration was present.
    Skipped,
    /// A matching registration was removed or rewritten.
    Applied,
}

/// Reconciles the Snow Shot Run value under `base\subkey` for uninstall and
/// migration. Keys that cannot be opened for reading cannot contain a
/// verifiable registration and are skipped; only a verified match that cannot
/// be modified is an error.
pub fn reconcile_startup_run_value(
    base: HKEY,
    subkey: &str,
    expected_command: &str,
    replacement_command: Option<&str>,
) -> Result<Reconciled> {
    unsafe {
        let mut run = HKEY::default();
        if RegOpenKeyExW(
            base,
            PCWSTR(wide_str(subkey).as_ptr()),
            None,
            KEY_QUERY_VALUE,
            &mut run,
        )
        .0 != 0
        {
            return Ok(Reconciled::Skipped);
        }
        let command = read_reg_sz(run, "", Some("SnowShot"), false);
        let matches = command.is_some_and(|command| paths_ci_eq(&command, expected_command));
        let _ = RegCloseKey(run);
        if !matches {
            return Ok(Reconciled::Skipped);
        }
        let mut write = HKEY::default();
        let opened = RegOpenKeyExW(
            base,
            PCWSTR(wide_str(subkey).as_ptr()),
            None,
            KEY_SET_VALUE,
            &mut write,
        );
        if opened.0 == 2 {
            // ERROR_FILE_NOT_FOUND
            return Ok(Reconciled::Skipped);
        }
        if opened.0 != 0 {
            return Err(windows_error(hresult_from_win32(opened.0)));
        }
        let result = match replacement_command {
            None => RegDeleteValueW(write, PCWSTR(wide_str("SnowShot").as_ptr())),
            Some(replacement) => {
                let mut data = wide_str(replacement);
                data.pop();
                RegSetValueExW(
                    write,
                    PCWSTR(wide_str("SnowShot").as_ptr()),
                    None,
                    REG_SZ,
                    Some(std::slice::from_raw_parts(
                        data.as_ptr().cast::<u8>(),
                        (data.len() + 1) * std::mem::size_of::<u16>(),
                    )),
                )
            }
        };
        let _ = RegCloseKey(write);
        if result.0 != 0 {
            return Err(windows_error(hresult_from_win32(result.0)));
        }
        Ok(Reconciled::Applied)
    }
}

/// Enumerates subkey names under a hive key.
pub fn enumerate_subkeys(base: HKEY, subkey: Option<&str>) -> Vec<String> {
    let mut names = Vec::new();
    unsafe {
        let mut key = base;
        let mut opened = HKEY::default();
        if let Some(subkey) = subkey {
            if RegOpenKeyExW(
                base,
                PCWSTR(wide_str(subkey).as_ptr()),
                None,
                KEY_READ,
                &mut opened,
            )
            .0 != 0
            {
                return names;
            }
            key = opened;
        }
        let mut name = [0u16; 256];
        for index in 0u32.. {
            let mut length = name.len() as u32;
            let status = RegEnumKeyExW(
                key,
                index,
                Some(windows::core::PWSTR(name.as_mut_ptr())),
                &mut length,
                None,
                None,
                None,
                None,
            );
            if status.0 == 259 {
                // ERROR_NO_MORE_ITEMS
                break;
            }
            if status.0 != 0 {
                break;
            }
            names.push(String::from_utf16_lossy(&name[..length as usize]));
        }
        if key != base {
            let _ = RegCloseKey(key);
        }
    }
    names
}

/// Reads `ProfileImagePath` for one profile SID.
pub fn profile_image_path(profiles: HKEY, sid: &str) -> Option<String> {
    read_reg_sz(profiles, sid, Some("ProfileImagePath"), false)
        .map(|path| path.trim_end_matches('\0').to_owned())
}

/// Flushes a loaded hive after a rewrite.
pub fn flush_key(key: HKEY) -> Result<()> {
    use windows::Win32::System::Registry::RegFlushKey;
    let status = unsafe { RegFlushKey(key) };
    if status.0 != 0 {
        return Err(windows_error(hresult_from_win32(status.0)));
    }
    Ok(())
}

pub const RUN_KEY: &str = RUN_SUBKEY;

/// Whether a user hive is already mounted under HKEY_USERS.
pub fn hive_loaded(sid: &str) -> bool {
    unsafe {
        let mut key = HKEY::default();
        let opened = RegOpenKeyExW(
            HKEY_USERS,
            PCWSTR(wide_str(sid).as_ptr()),
            None,
            KEY_READ,
            &mut key,
        );
        if opened.0 == 0 {
            let _ = RegCloseKey(key);
            return true;
        }
        false
    }
}

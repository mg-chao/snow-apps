use crate::error::{Result, UpdateError, io_error, require};
use path_clean::PathClean;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::time::Duration;
use windows::Win32::Foundation::{
    CloseHandle, ERROR_ACCESS_DENIED, ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_SUCCESS,
    HANDLE, WAIT_OBJECT_0, WIN32_ERROR,
};
use windows::Win32::Globalization::{CSTR_EQUAL, CompareStringOrdinal};
use windows::Win32::Storage::FileSystem::{
    FILE_ATTRIBUTE_REPARSE_POINT, GetFileAttributesW, INVALID_FILE_ATTRIBUTES,
    MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH, MoveFileExW,
};
use windows::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, KEY_CREATE_SUB_KEY, KEY_QUERY_VALUE,
    KEY_SET_VALUE, KEY_WOW64_32KEY, REG_OPTION_NON_VOLATILE, REG_SZ, REG_VALUE_TYPE, RRF_RT_REG_SZ,
    RRF_SUBKEY_WOW6432KEY, RegCloseKey, RegCreateKeyExW, RegGetValueW, RegOpenKeyExW,
    RegSetValueExW,
};
use windows::Win32::System::Threading::{
    OpenProcess, PROCESS_ACCESS_RIGHTS, PROCESS_QUERY_LIMITED_INFORMATION,
    QueryFullProcessImageNameW, WaitForSingleObject,
};
use windows::Win32::UI::Shell::{
    SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW, ShellExecuteExW,
};
use windows::Win32::UI::WindowsAndMessaging::SW_HIDE;
use windows::core::{PCWSTR, PWSTR};

// SYNCHRONIZE is a standard access right rather than a process-specific right. The
// windows crate does not expose it through every selected feature combination.
const SYNCHRONIZE_ACCESS: PROCESS_ACCESS_RIGHTS = PROCESS_ACCESS_RIGHTS(0x0010_0000);

fn wide(value: impl AsRef<OsStr>) -> Vec<u16> {
    value
        .as_ref()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn absolute(path: &Path) -> PathBuf {
    let path = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir().unwrap_or_default().join(path)
    };
    path.clean()
}

pub fn path_eq(left: &Path, right: &Path) -> bool {
    let left = absolute(left).as_os_str().encode_wide().collect::<Vec<_>>();
    let right = absolute(right)
        .as_os_str()
        .encode_wide()
        .collect::<Vec<_>>();
    unsafe { CompareStringOrdinal(&left, &right, true) == CSTR_EQUAL }
}

pub fn replace_file(source: &Path, destination: &Path) -> Result<()> {
    let source = wide(absolute(source));
    let destination = wide(absolute(destination));
    unsafe {
        MoveFileExW(
            PCWSTR(source.as_ptr()),
            PCWSTR(destination.as_ptr()),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
        .map_err(|error| {
            io_error(
                "update_file_replace_failed",
                "Could not replace an update file; close applications using it",
                error,
            )
        })
    }
}

pub fn path_has_reparse(path: &Path) -> bool {
    let path = wide(absolute(path));
    let attributes = unsafe { GetFileAttributesW(PCWSTR(path.as_ptr())) };
    attributes != INVALID_FILE_ATTRIBUTES && attributes & FILE_ATTRIBUTE_REPARSE_POINT.0 != 0
}

const INSTALL_KEY: &str = "Software\\Snow Apps\\SnowShot";
const UNINSTALL_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowShot";

fn registry_error(hive: HKEY, key: &str, operation: &str, status: WIN32_ERROR) -> UpdateError {
    let hive = if hive == HKEY_LOCAL_MACHINE {
        "HKLM"
    } else {
        "HKCU"
    };
    UpdateError::new(
        "registered_version_update_failed",
        "Could not update the registered application version",
    )
    .detail(format!(
        "{operation} {hive}\\{key} (32-bit view): Win32 error {}",
        status.0
    ))
}

fn read_registry_string(hive: HKEY, key: &str, name: &str) -> Result<Option<String>> {
    let subkey = wide(key);
    let name = wide(name);
    let mut buffer = vec![0_u16; 32_768];
    let mut bytes = (buffer.len() * size_of::<u16>()) as u32;
    let status = unsafe {
        RegGetValueW(
            hive,
            PCWSTR(subkey.as_ptr()),
            PCWSTR(name.as_ptr()),
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY,
            None,
            Some(buffer.as_mut_ptr().cast()),
            Some(&mut bytes),
        )
    };
    if status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND {
        return Ok(None);
    }
    if status != ERROR_SUCCESS {
        return Err(registry_error(hive, key, "RegGetValueW", status));
    }
    let length = bytes as usize / size_of::<u16>();
    Ok(Some(String::from_utf16_lossy(
        &buffer[..length.saturating_sub(1)],
    )))
}

fn registered_hive(root: &Path) -> Result<Option<HKEY>> {
    let mut discovery_error = None;
    for hive in [HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER] {
        match read_registry_string(hive, INSTALL_KEY, "") {
            Ok(Some(registered)) if path_eq(Path::new(&registered), root) => return Ok(Some(hive)),
            Err(error) => discovery_error = Some(error),
            _ => {}
        }
    }
    // An inaccessible registration is not evidence of a portable installation.
    match discovery_error {
        Some(error) => Err(error),
        None => Ok(None),
    }
}

pub fn registered_target_matches(root: &Path) -> Result<bool> {
    registered_hive(root).map(|hive| hive.is_some())
}

// No registry mutation: for a missing key, check creation rights on its nearest
// existing ancestor. A successful check is repeated in the elevated bootstrap.
pub fn registered_version_requires_elevation(root: &Path) -> Result<bool> {
    let Some(hive) = registered_hive(root)? else {
        return Ok(false);
    };
    let mut path = UNINSTALL_KEY;
    let mut rights = KEY_QUERY_VALUE | KEY_SET_VALUE;
    loop {
        let subkey = wide(path);
        let mut key = HKEY::default();
        let status = unsafe {
            RegOpenKeyExW(
                hive,
                PCWSTR(subkey.as_ptr()),
                None,
                rights | KEY_WOW64_32KEY,
                &mut key,
            )
        };
        if status == ERROR_SUCCESS {
            let _ = unsafe { RegCloseKey(key) };
            return Ok(false);
        }
        if status == ERROR_ACCESS_DENIED {
            return Ok(true);
        }
        if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
            && let Some((parent, _)) = path.rsplit_once('\\')
        {
            path = parent;
            rights = KEY_CREATE_SUB_KEY;
            continue;
        }
        return Err(registry_error(hive, path, "RegOpenKeyExW", status));
    }
}

pub fn write_registered_version(root: &Path, version: &str) -> Result<()> {
    let Some(hive) = registered_hive(root)? else {
        return Ok(());
    };
    // Also lets legacy recovery finish when a denied write left the old value intact.
    if read_registry_string(hive, UNINSTALL_KEY, "DisplayVersion")?.as_deref() == Some(version) {
        return Ok(());
    }
    let subkey = wide(UNINSTALL_KEY);
    let mut key = HKEY::default();
    let status = unsafe {
        RegCreateKeyExW(
            hive,
            PCWSTR(subkey.as_ptr()),
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_WOW64_32KEY,
            None,
            &mut key,
            None,
        )
    };
    if status != ERROR_SUCCESS {
        return Err(registry_error(
            hive,
            UNINSTALL_KEY,
            "RegCreateKeyExW",
            status,
        ));
    }
    let name = wide("DisplayVersion");
    let value = wide(version);
    let status = unsafe {
        RegSetValueExW(
            key,
            PCWSTR(name.as_ptr()),
            None,
            REG_VALUE_TYPE(REG_SZ.0),
            Some(std::slice::from_raw_parts(
                value.as_ptr().cast::<u8>(),
                value.len() * 2,
            )),
        )
    };
    let _ = unsafe { RegCloseKey(key) };
    if status != ERROR_SUCCESS {
        return Err(registry_error(
            hive,
            UNINSTALL_KEY,
            "RegSetValueExW",
            status,
        ));
    }
    Ok(())
}

struct Handle(HANDLE);

impl Drop for Handle {
    fn drop(&mut self) {
        if !self.0.is_invalid() {
            unsafe { CloseHandle(self.0).ok() };
        }
    }
}

fn process_path(process: HANDLE) -> Option<PathBuf> {
    let mut buffer = vec![0_u16; 32_768];
    let mut length = buffer.len() as u32;
    unsafe {
        QueryFullProcessImageNameW(
            process,
            Default::default(),
            PWSTR(buffer.as_mut_ptr()),
            &mut length,
        )
    }
    .ok()
    .map(|_| PathBuf::from(String::from_utf16_lossy(&buffer[..length as usize])))
}

pub fn process_path_for_pid(pid: u32) -> Result<PathBuf> {
    let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid) }
        .map(Handle)
        .map_err(|error| {
            io_error(
                "application_process_open_failed",
                "Could not open the application process",
                error,
            )
        })?;
    process_path(process.0).ok_or_else(|| {
        UpdateError::new(
            "application_process_mismatch",
            "The update target does not match its application process",
        )
    })
}

pub struct ValidatedProcess(Handle);

impl ValidatedProcess {
    pub fn wait_for_exit(&self, timeout: Duration) -> Result<()> {
        let result = unsafe {
            WaitForSingleObject(self.0.0, timeout.as_millis().min(u32::MAX as u128) as u32)
        };
        require(
            result == WAIT_OBJECT_0,
            "application_exit_timeout",
            "The application did not exit; the update was cancelled",
        )
    }
}

pub fn open_validated_process(pid: u32, expected: &Path) -> Result<ValidatedProcess> {
    let process = unsafe {
        OpenProcess(
            SYNCHRONIZE_ACCESS | PROCESS_QUERY_LIMITED_INFORMATION,
            false,
            pid,
        )
    }
    .map(Handle)
    .map_err(|error| {
        io_error(
            "application_process_open_failed",
            "Could not open the application process",
            error,
        )
    })?;
    let actual = process_path(process.0).unwrap_or_default();
    require(
        path_eq(&actual, expected),
        "application_process_mismatch",
        "The update target does not match its application process",
    )?;
    Ok(ValidatedProcess(process))
}

fn quote_argument(argument: &str) -> Result<String> {
    require(
        !argument.contains('"'),
        "invalid_updater_argument",
        "Invalid updater command argument",
    )?;
    Ok(format!("\"{}\"", argument.trim_end_matches('\\')))
}

pub fn launch_elevated(executable: &Path, arguments: &[String]) -> Result<bool> {
    let executable = wide(executable);
    let verb = wide("runas");
    let parameters = arguments
        .iter()
        .map(|argument| quote_argument(argument))
        .collect::<Result<Vec<_>>>()?
        .join(" ");
    let parameters = wide(parameters);
    let mut info = SHELLEXECUTEINFOW {
        cbSize: size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
        lpVerb: PCWSTR(verb.as_ptr()),
        lpFile: PCWSTR(executable.as_ptr()),
        lpParameters: PCWSTR(parameters.as_ptr()),
        nShow: SW_HIDE.0,
        ..Default::default()
    };
    if unsafe { ShellExecuteExW(&mut info) }.is_err() {
        return Ok(false);
    }
    if !info.hProcess.is_invalid() {
        unsafe { CloseHandle(info.hProcess).ok() };
    }
    Ok(true)
}

pub fn remove_installation_startup(_root: &Path) -> Result<()> {
    super::windows_startup::remove_installation_startup(_root)
}

pub fn migrate_installation_startup(previous: &Path, root: &Path) -> Result<()> {
    super::windows_startup::migrate_installation_startup(previous, root)
}

pub fn launch_on_interactive_desktop(executable: &Path) -> Result<bool> {
    super::windows_startup::launch_on_interactive_desktop(executable)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    #[test]
    fn validated_process_handle_survives_process_exit() {
        let command = std::env::var_os("ComSpec").unwrap_or_else(|| "cmd.exe".into());
        let mut child = Command::new(command)
            .args(["/d", "/c", "ping -n 2 127.0.0.1 >nul"])
            .spawn()
            .unwrap();
        let expected = process_path_for_pid(child.id()).unwrap();
        let process = open_validated_process(child.id(), &expected).unwrap();
        assert!(child.wait().unwrap().success());
        drop(child);
        process.wait_for_exit(Duration::from_secs(1)).unwrap();
    }
}

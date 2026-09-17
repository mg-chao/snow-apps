//! Process launching, elevation, and identity checks.

use super::{last_windows_error, path_from_wide, wide};
use crate::errors::{Error, Result, require};
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use windows::Win32::Foundation::{CloseHandle, WAIT_OBJECT_0};
use windows::Win32::System::LibraryLoader::GetModuleFileNameW;
use windows::Win32::System::Threading::{
    GetCurrentProcess, GetExitCodeProcess, OpenProcess, PROCESS_CREATION_FLAGS,
    PROCESS_NAME_FORMAT, QueryFullProcessImageNameW, STARTUPINFOW, WaitForSingleObject,
};
use windows::Win32::UI::Shell::{SEE_MASK_NOASYNC, SEE_MASK_NOCLOSEPROCESS, ShellExecuteExW};
use windows::Win32::UI::WindowsAndMessaging::SW_HIDE;
use windows::core::PCWSTR;

pub fn current_process_image() -> PathBuf {
    let mut buffer = [0u16; 32768];
    let length = unsafe { GetModuleFileNameW(None, &mut buffer) } as usize;
    path_from_wide(&buffer[..length])
}

pub fn process_image(pid: u32) -> Option<PathBuf> {
    use windows::Win32::System::Threading::PROCESS_QUERY_LIMITED_INFORMATION;
    unsafe {
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid).ok()?;
        let mut buffer = [0u16; 32768];
        let mut length = buffer.len() as u32;
        let ok = QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_FORMAT(0),
            windows::core::PWSTR(buffer.as_mut_ptr()),
            &mut length,
        )
        .is_ok()
            && CloseHandle(process).is_ok();
        ok.then(|| path_from_wide(&buffer[..length as usize]))
    }
}

pub fn wait_for_exit(pid: u32, timeout_ms: u32) -> bool {
    use windows::Win32::Foundation::WAIT_TIMEOUT;
    use windows::Win32::System::Threading::PROCESS_SYNCHRONIZE;
    unsafe {
        let Ok(process) = OpenProcess(PROCESS_SYNCHRONIZE, false, pid) else {
            return false;
        };
        let waited = WaitForSingleObject(process, timeout_ms);
        let exited = waited == WAIT_OBJECT_0;
        let _ = CloseHandle(process);
        waited != WAIT_TIMEOUT && exited || exited
    }
}

/// Spawns a probe process and collects its exit code within the deadline.
pub fn spawn_and_wait(exe: &Path, args: &[String], finish_timeout_ms: u32) -> Option<i32> {
    use windows::Win32::System::Threading::{
        CreateProcessW, PROCESS_INFORMATION, TerminateProcess,
    };
    unsafe {
        let mut command = format!("\"{}\"", exe.display());
        for argument in args {
            command.push(' ');
            command.push_str(&quote_argument(argument));
        }
        let mut wide_command = wide(OsStr::new(&command));
        let startup = STARTUPINFOW {
            cb: std::mem::size_of::<STARTUPINFOW>() as u32,
            ..Default::default()
        };
        let mut information = PROCESS_INFORMATION::default();
        let created = CreateProcessW(
            None,
            Some(windows::core::PWSTR(wide_command.as_mut_ptr())),
            None,
            None,
            false,
            PROCESS_CREATION_FLAGS(0),
            None,
            None,
            &startup,
            &mut information,
        );
        if !created.is_ok() {
            return None;
        }
        let _ = CloseHandle(information.hThread);
        let handle = information.hProcess;
        let finished = WaitForSingleObject(handle, finish_timeout_ms) == WAIT_OBJECT_0;
        if !finished {
            let _ = TerminateProcess(handle, 1);
            let _ = WaitForSingleObject(handle, 5000);
            let _ = CloseHandle(handle);
            return None;
        }
        let mut code = 0u32;
        let _ = GetExitCodeProcess(handle, &mut code);
        let _ = CloseHandle(handle);
        Some(code as i32)
    }
}

fn quote_argument(argument: &str) -> String {
    format!("\"{argument}\"")
}

/// Starts a detached process that outlives this one.
pub fn start_detached(exe: &Path, args: &[OsString], working_dir: &Path) -> bool {
    use windows::Win32::System::Threading::{CreateProcessW, PROCESS_INFORMATION};
    unsafe {
        let mut command = format!("\"{}\"", exe.display());
        for argument in args {
            command.push(' ');
            // Arguments are file names, identifiers, and switches; quote
            // every value so paths with spaces survive the command line.
            command.push_str(&format!("\"{}\"", argument.to_string_lossy()));
        }
        let mut wide_command = wide(OsStr::new(&command));
        let startup = STARTUPINFOW {
            cb: std::mem::size_of::<STARTUPINFOW>() as u32,
            ..Default::default()
        };
        let mut information = PROCESS_INFORMATION::default();
        let created = CreateProcessW(
            None,
            Some(windows::core::PWSTR(wide_command.as_mut_ptr())),
            None,
            None,
            false,
            DETACHED_CREATION,
            None,
            PCWSTR(wide(working_dir.as_os_str()).as_ptr()),
            &startup,
            &mut information,
        );
        if !created.is_ok() {
            return false;
        }
        let _ = CloseHandle(information.hThread);
        let _ = CloseHandle(information.hProcess);
        true
    }
}

const DETACHED_CREATION: PROCESS_CREATION_FLAGS = PROCESS_CREATION_FLAGS(0x00000008 | 0x00000200);

/// Elevates through the UAC consent dialog with the historical quoting
/// rules; returns whether the elevated process launched.
pub fn launch_elevated(exe: &Path, args: &[OsString]) -> Result<bool> {
    use windows::Win32::UI::Shell::SHELLEXECUTEINFOW;
    let mut quoted = Vec::with_capacity(args.len());
    for argument in args {
        let text = argument.to_string_lossy();
        require(!text.contains('"'), "Invalid updater command argument")?;
        // Arguments are file names, identifiers, and switches; trailing
        // backslash escapes inside quotes must be avoided.
        let trimmed = text.trim_end_matches('\\');
        quoted.push(format!("\"{trimmed}\""));
    }
    let parameters = quoted.join(" ");
    unsafe {
        let mut info = SHELLEXECUTEINFOW {
            cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
            fMask: SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
            lpVerb: windows::core::PCWSTR(crate::platform::wide_str("runas").as_ptr()),
            lpFile: PCWSTR(wide(exe.as_os_str()).as_ptr()),
            lpParameters: PCWSTR(crate::platform::wide_str(&parameters).as_ptr()),
            nShow: SW_HIDE.0,
            ..Default::default()
        };
        if ShellExecuteExW(&mut info).is_err() {
            return Ok(false);
        }
        if !info.hProcess.is_invalid() {
            let _ = CloseHandle(info.hProcess);
        }
        Ok(true)
    }
}

/// Terminates this process immediately without unwinding, simulating power
/// loss for the crash-recovery tests.
pub fn terminate_self(exit_code: u32) -> ! {
    use windows::Win32::System::Threading::TerminateProcess;
    unsafe {
        let _ = TerminateProcess(GetCurrentProcess(), exit_code);
    }
    unreachable!("TerminateProcess on the current process cannot return")
}

pub fn last_error_debug() -> Error {
    last_windows_error()
}

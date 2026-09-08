mod clipboard;
mod native;
mod uia;

#[cfg(test)]
mod fixture;

use std::time::Instant;

use windows::Win32::Foundation::{CloseHandle, E_ACCESSDENIED, HWND};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
use windows::Win32::System::Threading::{
    OpenProcess, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, QueryFullProcessImageNameW,
};
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::PWSTR;

use crate::model::*;
use crate::policy::{Backend, Context, Probe, terminal_executable};

pub(super) fn hwnd(value: usize) -> HWND {
    HWND(value as *mut _)
}

pub(super) fn api_error(operation: &'static str, error: windows::core::Error) -> SelectionError {
    let code = error.code();
    let kind = if code == E_ACCESSDENIED {
        ErrorKind::AccessDenied
    } else if code.0 == 0x80131505u32 as i32 || code.0 == 0x800705B4u32 as i32 {
        ErrorKind::TimedOut
    } else if code.0 == 0x80040201u32 as i32 {
        ErrorKind::TargetUnavailable
    } else {
        ErrorKind::NativeApi
    };
    SelectionError {
        kind,
        operation,
        native_code: Some(code.0),
        clipboard_status: ClipboardStatus::Unchanged,
    }
}

pub(crate) fn capture_source() -> Result<SourceWindow, SelectionError> {
    // These APIs query kernel/window-manager state, not target window procedures.
    unsafe {
        let window = GetForegroundWindow();
        if window.is_invalid() {
            return Err(SelectionError::new(
                ErrorKind::NoForegroundWindow,
                "GetForegroundWindow",
            ));
        }
        let mut process_id = 0;
        let thread = GetWindowThreadProcessId(window, Some(&mut process_id));
        if thread == 0 || process_id == 0 {
            return Err(SelectionError::new(
                ErrorKind::TargetUnavailable,
                "foreground identity",
            ));
        }
        let mut info = GUITHREADINFO {
            cbSize: size_of::<GUITHREADINFO>() as u32,
            ..Default::default()
        };
        GetGUIThreadInfo(thread, &mut info).map_err(|e| api_error("GetGUIThreadInfo", e))?;
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, process_id)
            .map_err(|e| api_error("OpenProcess", e))?;
        let mut path = vec![0u16; 32768];
        let mut length = path.len() as u32;
        let query = QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_WIN32,
            PWSTR(path.as_mut_ptr()),
            &mut length,
        );
        let _ = CloseHandle(process);
        query.map_err(|e| api_error("QueryFullProcessImageNameW", e))?;
        let path = decode_utf16(&path[..length as usize], 131072)?;
        let executable = path.rsplit(['\\', '/']).next().unwrap_or(&path).to_owned();
        Ok(SourceWindow {
            window: window.0 as usize,
            process_id,
            focused_control: info.hwndFocus.0 as usize,
            executable,
        })
    }
}

pub(super) fn validate(context: &Context) -> Result<(), SelectionError> {
    context.check()?;
    unsafe {
        let window = hwnd(context.source.window);
        if !IsWindow(Some(window)).as_bool() {
            return Err(SelectionError::new(
                ErrorKind::TargetUnavailable,
                "source window",
            ));
        }
        let mut process_id = 0;
        let thread = GetWindowThreadProcessId(window, Some(&mut process_id));
        if GetForegroundWindow() != window || process_id != context.source.process_id {
            return Err(SelectionError::new(
                ErrorKind::TargetChanged,
                "foreground identity",
            ));
        }
        let mut info = GUITHREADINFO {
            cbSize: size_of::<GUITHREADINFO>() as u32,
            ..Default::default()
        };
        GetGUIThreadInfo(thread, &mut info).map_err(|e| api_error("GetGUIThreadInfo", e))?;
        if info.hwndFocus.0 as usize != context.source.focused_control {
            return Err(SelectionError::new(
                ErrorKind::TargetChanged,
                "native focus",
            ));
        }
    }
    Ok(())
}

pub(super) fn class_name(window: HWND) -> String {
    let mut name = [0u16; 256];
    let length = unsafe { GetClassNameW(window, &mut name) }.max(0) as usize;
    String::from_utf16_lossy(&name[..length])
}

pub(super) fn check_copy_allowed(context: &Context) -> Result<(), SelectionError> {
    let native_class = class_name(hwnd(context.source.window));
    if terminal_executable(&context.source.executable)
        || native_class.eq_ignore_ascii_case("ConsoleWindowClass")
        || native_class.eq_ignore_ascii_case("CASCADIA_HOSTING_WINDOW_CLASS")
    {
        return Err(SelectionError::new(
            ErrorKind::CopyBlocked,
            "terminal Copy safeguard",
        ));
    }
    native::check_password(hwnd(context.source.focused_control))
}

struct Apartment;
impl Apartment {
    fn new() -> Result<Self, SelectionError> {
        unsafe { CoInitializeEx(None, COINIT_MULTITHREADED).ok() }
            .map_err(|e| api_error("CoInitializeEx", e))?;
        Ok(Self)
    }
}
impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe { CoUninitialize() };
    }
}

pub(crate) struct Worker {
    // COM references must drop before the apartment.
    uia: Option<uia::Automation>,
    _apartment: Apartment,
}

impl Worker {
    pub fn new() -> Result<Self, SelectionError> {
        Ok(Self {
            uia: None,
            _apartment: Apartment::new()?,
        })
    }
}

impl Backend for Worker {
    fn validate_target(&mut self, context: &Context) -> Result<(), SelectionError> {
        validate(context)
    }

    fn uia(&mut self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
        native::check_password(hwnd(context.source.focused_control))?;
        if self.uia.is_none() {
            self.uia = Some(uia::Automation::new()?);
        }
        self.uia
            .as_ref()
            .ok_or_else(|| SelectionError::new(ErrorKind::WorkerUnavailable, "UIA initialization"))?
            .capture(context, deadline)
    }

    fn native(&mut self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
        native::capture(context, deadline)
    }

    fn copy(&mut self, context: &Context) -> CaptureResult {
        check_copy_allowed(context)?;
        clipboard::capture(context.clone())
    }
}

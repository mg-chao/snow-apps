//! Named-pipe line protocol compatible with `QLocalServer`/`QLocalSocket`
//! peers, plus the owner-and-administrators ACL server used by the broker.

use super::wide_str;
use crate::errors::{Error, Result};
use std::time::{Duration, Instant};
use windows::Win32::Foundation::{CloseHandle, GetLastError, LocalFree, WAIT_OBJECT_0};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, FILE_GENERIC_READ,
    FILE_GENERIC_WRITE, OPEN_EXISTING, PIPE_ACCESS_DUPLEX, ReadFile, WriteFile,
};
use windows::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};
use windows::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, GetNamedPipeClientProcessId, GetNamedPipeServerProcessId,
    PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES,
    PIPE_WAIT, WaitNamedPipeW,
};
use windows::Win32::System::Threading::{CreateEventW, SetEvent, WaitForSingleObject};
use windows::core::PCWSTR;

const MAX_LINE: usize = 4096;
const ERROR_IO_PENDING: u32 = 997;
const ERROR_PIPE_CONNECTED: u32 = 535;
const ERROR_PIPE_BUSY: u32 = 231;

/// RAII wrapper over a kernel HANDLE.
struct OwnedHandle(windows::Win32::Foundation::HANDLE);

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

/// RAII manual-reset event for overlapped waits.
struct Event(OwnedHandle);

impl Event {
    fn new() -> Option<Self> {
        let event = unsafe { CreateEventW(None, true, false, None) }.ok()?;
        Some(Event(OwnedHandle(event)))
    }
}

pub struct PipeStream {
    handle: OwnedHandle,
}

impl PipeStream {
    pub fn server_process_id(&self) -> Option<u32> {
        let mut pid = 0u32;
        if unsafe { GetNamedPipeServerProcessId(self.handle.0, &mut pid) }.is_ok() {
            Some(pid)
        } else {
            None
        }
    }

    pub fn client_process_id(&self) -> Option<u32> {
        let mut pid = 0u32;
        if unsafe { GetNamedPipeClientProcessId(self.handle.0, &mut pid) }.is_ok() {
            Some(pid)
        } else {
            None
        }
    }

    pub fn write_line(&self, line: &[u8], timeout_ms: u32) -> Result<()> {
        let mut payload = Vec::with_capacity(line.len() + 1);
        payload.extend_from_slice(line);
        payload.push(b'\n');
        overlapped_write(self.handle.0, &payload, timeout_ms)
            .map_err(|_| Error::fixed("Could not send updater status"))
    }

    pub fn read_line(&self, timeout_ms: u32) -> Result<Vec<u8>> {
        let deadline = Instant::now() + Duration::from_millis(timeout_ms as u64);
        let mut line = Vec::with_capacity(128);
        let mut chunk = [0u8; MAX_LINE];
        while line.len() < MAX_LINE {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(Error::fixed("The update handoff was not acknowledged"));
            }
            let count = overlapped_read(self.handle.0, &mut chunk, remaining.as_millis() as u32)
                .map_err(|_| Error::fixed("The update handoff was not acknowledged"))?;
            if count == 0 {
                return Err(Error::fixed("The update handoff was not acknowledged"));
            }
            match chunk[..count].iter().position(|byte| *byte == b'\n') {
                Some(index) => {
                    line.extend_from_slice(&chunk[..index]);
                    return Ok(trimmed(line));
                }
                None => line.extend_from_slice(&chunk[..count]),
            }
        }
        Ok(trimmed(line))
    }
}

fn trimmed(mut line: Vec<u8>) -> Vec<u8> {
    while line.last().is_some_and(|byte| byte.is_ascii_whitespace()) {
        line.pop();
    }
    while line.first().is_some_and(|byte| byte.is_ascii_whitespace()) {
        line.remove(0);
    }
    line
}

/// Connects to a `QLocalServer`-style pipe by name (without prefix).
pub fn connect(name: &str, timeout_ms: u32) -> Result<PipeStream> {
    let path = format!("\\\\.\\pipe\\{name}");
    let deadline = Instant::now() + Duration::from_millis(timeout_ms as u64);
    loop {
        let handle = unsafe {
            CreateFileW(
                PCWSTR(wide_str(&path).as_ptr()),
                (FILE_GENERIC_READ | FILE_GENERIC_WRITE).0,
                Default::default(),
                None,
                OPEN_EXISTING,
                Default::default(),
                None,
            )
        };
        if let Ok(handle) = handle {
            return Ok(PipeStream {
                handle: OwnedHandle(handle),
            });
        }
        let busy = unsafe { GetLastError().0 } == ERROR_PIPE_BUSY;
        let remaining = deadline.saturating_duration_since(Instant::now());
        if !busy || remaining.is_zero() {
            return Err(Error::fixed("Could not contact the update coordinator"));
        }
        unsafe {
            let _ = WaitNamedPipeW(
                PCWSTR(wide_str(&path).as_ptr()),
                remaining.as_millis().min(1000) as u32,
            );
        }
    }
}

fn overlapped_write(
    handle: windows::Win32::Foundation::HANDLE,
    payload: &[u8],
    timeout_ms: u32,
) -> std::result::Result<(), ()> {
    let Some(event) = Event::new() else {
        return Err(());
    };
    let mut overlapped = OVERLAPPED {
        hEvent: event.0.0,
        ..Default::default()
    };
    let mut written = 0u32;
    let completed = unsafe {
        WriteFile(
            handle,
            Some(payload),
            Some(&mut written),
            Some(&mut overlapped),
        )
    };
    if completed.is_err() && unsafe { GetLastError().0 } != ERROR_IO_PENDING {
        return Err(());
    }
    if unsafe { WaitForSingleObject(event.0.0, timeout_ms) } != WAIT_OBJECT_0 {
        unsafe {
            let _ = CancelIoEx(handle, Some(&overlapped));
        }
        return Err(());
    }
    let mut transferred = 0u32;
    if unsafe { GetOverlappedResult(handle, &overlapped, &mut transferred, true) }.is_ok() {
        Ok(())
    } else {
        Err(())
    }
}

fn overlapped_read(
    handle: windows::Win32::Foundation::HANDLE,
    buffer: &mut [u8],
    timeout_ms: u32,
) -> std::result::Result<usize, ()> {
    let Some(event) = Event::new() else {
        return Err(());
    };
    let mut overlapped = OVERLAPPED {
        hEvent: event.0.0,
        ..Default::default()
    };
    let mut read = 0u32;
    let completed =
        unsafe { ReadFile(handle, Some(buffer), Some(&mut read), Some(&mut overlapped)) };
    if completed.is_err() && unsafe { GetLastError().0 } != ERROR_IO_PENDING {
        return Err(());
    }
    if unsafe { WaitForSingleObject(event.0.0, timeout_ms) } != WAIT_OBJECT_0 {
        unsafe {
            let _ = CancelIoEx(handle, Some(&overlapped));
        }
        return Err(());
    }
    let mut transferred = 0u32;
    if unsafe { GetOverlappedResult(handle, &overlapped, &mut transferred, true) }.is_ok() {
        Ok(transferred as usize)
    } else {
        Err(())
    }
}

fn current_user_sid() -> Option<String> {
    use windows::Win32::Foundation::LocalFree;
    use windows::Win32::Security::Authorization::ConvertSidToStringSidW;
    use windows::Win32::Security::{GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenUser};
    use windows::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
    unsafe {
        let mut token = Default::default();
        if !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token).is_ok() {
            return None;
        }
        let mut size = 0u32;
        let _ = GetTokenInformation(token, TokenUser, None, 0, &mut size);
        let mut buffer = vec![0u8; size as usize];
        let read = GetTokenInformation(
            token,
            TokenUser,
            Some(buffer.as_mut_ptr().cast()),
            size,
            &mut size,
        )
        .is_ok();
        let _ = CloseHandle(token);
        if !read {
            return None;
        }
        let user = buffer.as_ptr().cast::<TOKEN_USER>();
        let mut sid = windows::core::PWSTR::null();
        if !ConvertSidToStringSidW((*user).User.Sid, &mut sid).is_ok() {
            return None;
        }
        let text = sid.to_string().ok()?;
        let _ = LocalFree(Some(windows::Win32::Foundation::HLOCAL(
            sid.as_ptr().cast(),
        )));
        Some(text)
    }
}

/// One armed pipe instance: the kernel owns the OVERLAPPED while the
/// ConnectNamedPipe is pending, so it shares the allocation with the pipe
/// and event and is never reused across operations.
struct PendingAccept {
    pipe: OwnedHandle,
    event: Event,
    overlapped: Box<OVERLAPPED>,
}

impl PendingAccept {
    fn decompose(self) -> (OwnedHandle, Event, Box<OVERLAPPED>) {
        (self.pipe, self.event, self.overlapped)
    }
}

/// The broker's pipe server: same DACL shape as the previous
/// `PrivilegedLocalServer`, byte mode, remote clients rejected.
pub struct PrivilegedPipeServer {
    pipe_name: String,
    pending: Option<PendingAccept>,
}

impl PrivilegedPipeServer {
    pub fn listen(name: &str) -> Result<Self> {
        let mut server = PrivilegedPipeServer {
            pipe_name: name.to_owned(),
            pending: None,
        };
        server.arm(true)?;
        Ok(server)
    }

    fn arm(&mut self, first: bool) -> Result<()> {
        use windows::Win32::Security::Authorization::ConvertStringSecurityDescriptorToSecurityDescriptorW;
        use windows::Win32::Security::Authorization::SDDL_REVISION_1;
        use windows::Win32::Security::SECURITY_ATTRIBUTES;
        let sid = current_user_sid()
            .ok_or_else(|| Error::fixed("Could not create updater coordinator"))?;
        let sddl = format!("D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{sid})S:(ML;;NW;;;ME)");
        let mut descriptor = windows::Win32::Security::PSECURITY_DESCRIPTOR(std::ptr::null_mut());
        let converted = unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                PCWSTR(wide_str(&sddl).as_ptr()),
                SDDL_REVISION_1,
                &mut descriptor,
                None,
            )
        };
        if !converted.is_ok() {
            return Err(Error::fixed("Could not create updater coordinator"));
        }
        let pipe_path = format!("\\\\.\\pipe\\{}", self.pipe_name);
        let flags = if first {
            FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED
        } else {
            FILE_FLAG_OVERLAPPED
        };
        let pipe = unsafe {
            CreateNamedPipeW(
                PCWSTR(wide_str(&pipe_path).as_ptr()),
                PIPE_ACCESS_DUPLEX | flags,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                4096,
                4096,
                0,
                Some(&SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: descriptor.0,
                    bInheritHandle: false.into(),
                }),
            )
        };
        unsafe {
            let _ = LocalFree(Some(windows::Win32::Foundation::HLOCAL(descriptor.0)));
        }
        if pipe.is_invalid() {
            return Err(Error::fixed("Could not create updater coordinator"));
        }
        let Some(event) = Event::new() else {
            unsafe {
                let _ = CloseHandle(pipe);
            }
            return Err(Error::fixed("Could not create updater coordinator"));
        };
        let mut pending = PendingAccept {
            pipe: OwnedHandle(pipe),
            event,
            overlapped: Box::new(OVERLAPPED::default()),
        };
        pending.overlapped.hEvent = pending.event.0.0;
        let connected = unsafe { ConnectNamedPipe(pending.pipe.0, Some(&mut *pending.overlapped)) };
        if connected.is_err() {
            let code = unsafe { GetLastError().0 };
            if code != ERROR_IO_PENDING && code != ERROR_PIPE_CONNECTED {
                return Err(Error::fixed("Could not create updater coordinator"));
            }
            if code == ERROR_PIPE_CONNECTED {
                unsafe {
                    let _ = SetEvent(pending.event.0.0);
                }
            }
        }
        self.pending = Some(pending);
        Ok(())
    }

    /// Waits for the next connection until the deadline and hands it over
    /// after confirming the overlapped completion; a fresh instance is armed
    /// before returning so back-to-back connections both get served.
    pub fn accept(&mut self, timeout_ms: u32) -> Result<PipeStream> {
        let Some(pending) = self.pending.take() else {
            return Err(Error::fixed("Could not create updater coordinator"));
        };
        let deadline = Instant::now() + Duration::from_millis(timeout_ms as u64);
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero()
            || unsafe {
                WaitForSingleObject(
                    pending.event.0.0,
                    remaining.as_millis().min(u32::MAX as u128) as u32,
                )
            } != WAIT_OBJECT_0
        {
            // Restore the pending accept before reporting the timeout.
            self.pending = Some(pending);
            return Err(Error::fixed("The update helper timed out"));
        }
        let (pipe, event, overlapped) = pending.decompose();
        drop(event);
        let mut transferred = 0u32;
        let connected =
            unsafe { GetOverlappedResult(pipe.0, &*overlapped, &mut transferred, true) };
        self.arm(false)?;
        if connected.is_ok() {
            Ok(PipeStream { handle: pipe })
        } else {
            Err(Error::fixed("Could not create updater coordinator"))
        }
    }
}

impl Drop for PrivilegedPipeServer {
    fn drop(&mut self) {
        self.pending = None;
    }
}

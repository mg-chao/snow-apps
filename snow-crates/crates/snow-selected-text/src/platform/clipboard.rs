use std::sync::{OnceLock, mpsc};
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{
    GetLastError, GlobalFree, HANDLE, HGLOBAL, HWND, SetLastError, WIN32_ERROR,
};
use windows::Win32::Graphics::Gdi::*;
use windows::Win32::System::DataExchange::*;
use windows::Win32::System::Memory::*;
use windows::Win32::System::Ole::{CLIPBOARD_FORMAT, OleDuplicateData};
use windows::Win32::UI::Input::KeyboardAndMouse::*;
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::w;

use super::{api_error, check_copy_allowed, hwnd, validate};
use crate::clipboard::{
    ClipboardIo, CopiedText, Snapshot, clipboard_utf16, restoration_decision, transaction,
};
use crate::model::*;
use crate::policy::Context;

const MAX_SNAPSHOT: usize = 64 * 1024 * 1024;
const MAX_FORMATS: usize = 256;
const UNICODE_TEXT: u32 = 13;

struct Job {
    context: Context,
    reply: mpsc::Sender<CaptureResult>,
}

pub(super) fn capture(context: Context) -> CaptureResult {
    static BROKER: OnceLock<Result<mpsc::Sender<Job>, SelectionError>> = OnceLock::new();
    let sender = BROKER
        .get_or_init(|| {
            let (sender, receiver) = mpsc::channel::<Job>();
            std::thread::Builder::new()
                .name("snow-selected-text-clipboard".into())
                .spawn(move || {
                    let mut clipboard = NativeClipboard::new();
                    // This window belongs only to the clipboard thread, never the UIA MTA.
                    while let Ok(job) = receiver.recv() {
                        let result = match &mut clipboard {
                            Ok(clipboard) => transaction(clipboard, &job.context),
                            Err(error) => Err(error.clone()),
                        };
                        let _ = job.reply.send(result);
                    }
                })
                .map_err(|_| {
                    SelectionError::new(ErrorKind::WorkerUnavailable, "clipboard worker spawn")
                })?;
            Ok(sender)
        })
        .as_ref()
        .map_err(Clone::clone)?;
    let (reply, receiver) = mpsc::channel();
    sender
        .send(Job { context, reply })
        .map_err(|_| SelectionError::new(ErrorKind::WorkerUnavailable, "clipboard dispatch"))?;
    // Deliberately retain the process-wide busy lease until the clipboard worker finishes.
    // The public request handle independently enforces the caller's deadline.
    receiver
        .recv()
        .map_err(|_| SelectionError::new(ErrorKind::WorkerUnavailable, "clipboard worker"))?
}

struct Open;
impl Drop for Open {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseClipboard();
        }
    }
}

struct Memory(HGLOBAL);
impl Drop for Memory {
    fn drop(&mut self) {
        unsafe {
            let _ = GlobalFree(Some(self.0));
        }
    }
}

struct Format {
    id: u32,
    handle: HANDLE,
}

impl Drop for Format {
    fn drop(&mut self) {
        unsafe {
            match self.id {
                2 | 9 => {
                    let _ = DeleteObject(HGDIOBJ(self.handle.0));
                }
                14 => {
                    let _ = DeleteEnhMetaFile(Some(HENHMETAFILE(self.handle.0)));
                }
                3 => {
                    let memory = HGLOBAL(self.handle.0);
                    let picture = GlobalLock(memory).cast::<METAFILEPICT>();
                    if !picture.is_null() {
                        let _ = DeleteMetaFile((*picture).hMF);
                        let _ = GlobalUnlock(memory);
                    }
                    let _ = GlobalFree(Some(memory));
                }
                _ => {
                    let _ = GlobalFree(Some(HGLOBAL(self.handle.0)));
                }
            }
        }
    }
}

struct NativeClipboard {
    window: HWND,
}

impl NativeClipboard {
    fn new() -> Result<Self, SelectionError> {
        let window = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                w!("STATIC"),
                w!(""),
                WINDOW_STYLE::default(),
                0,
                0,
                0,
                0,
                Some(HWND_MESSAGE),
                None,
                None,
                None,
            )
        }
        .map_err(|e| api_error("clipboard message window", e))?;
        if let Err(error) = unsafe { AddClipboardFormatListener(window) } {
            unsafe {
                let _ = DestroyWindow(window);
            }
            return Err(api_error("AddClipboardFormatListener", error));
        }
        Ok(Self { window })
    }

    fn pump(&self) {
        unsafe {
            let mut message = MSG::default();
            // Drain notifications without dispatching arbitrary application messages.
            while PeekMessageW(
                &mut message,
                Some(self.window),
                WM_CLIPBOARDUPDATE,
                WM_CLIPBOARDUPDATE,
                PM_REMOVE,
            )
            .as_bool()
            {}
        }
    }

    fn pause(&self, deadline: Instant) {
        self.pump();
        std::thread::sleep(
            deadline
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(5)),
        );
    }

    fn open(&self, deadline: Instant) -> Result<Open, SelectionError> {
        loop {
            if unsafe { OpenClipboard(Some(self.window)) }.is_ok() {
                return Ok(Open);
            }
            if Instant::now() >= deadline {
                return Err(SelectionError::new(
                    ErrorKind::ClipboardBusy,
                    "OpenClipboard",
                ));
            }
            self.pause(deadline);
        }
    }

    fn attributable(&self, context: &Context) -> bool {
        let Ok(owner) = (unsafe { GetClipboardOwner() }) else {
            return false;
        };
        let mut process_id = 0;
        unsafe {
            GetWindowThreadProcessId(owner, Some(&mut process_id));
        }
        process_id == context.source.process_id
            || unsafe { GetAncestor(owner, GA_ROOT) } == hwnd(context.source.window)
            || unsafe { GetAncestor(owner, GA_ROOTOWNER) } == hwnd(context.source.window)
    }
}

impl Drop for NativeClipboard {
    fn drop(&mut self) {
        unsafe {
            let _ = RemoveClipboardFormatListener(self.window);
            let _ = DestroyWindow(self.window);
        }
    }
}

fn duplicate_memory(handle: HANDLE, remaining: usize) -> Result<(Memory, usize), SelectionError> {
    unsafe {
        let source = HGLOBAL(handle.0);
        let size = GlobalSize(source);
        if size == 0 || size > remaining {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "clipboard snapshot",
            ));
        }
        let destination =
            Memory(GlobalAlloc(GMEM_MOVEABLE, size).map_err(|e| api_error("GlobalAlloc", e))?);
        let input = GlobalLock(source);
        if input.is_null() {
            return Err(api_error(
                "GlobalLock source",
                windows::core::Error::from_thread(),
            ));
        }
        let output = GlobalLock(destination.0);
        if output.is_null() {
            let error = api_error("GlobalLock snapshot", windows::core::Error::from_thread());
            let _ = GlobalUnlock(source);
            return Err(error);
        }
        std::ptr::copy_nonoverlapping(input.cast::<u8>(), output.cast::<u8>(), size);
        let _ = GlobalUnlock(destination.0);
        let _ = GlobalUnlock(source);
        Ok((destination, size))
    }
}

fn byte_format(id: u32) -> bool {
    // Standard HGLOBAL formats and registered formats follow the clipboard allocation contract.
    // GDI handles, owner-display and private formats require application-specific lifetime rules.
    matches!(id, 1 | 4 | 6 | 7 | 8 | 10 | 11 | 13 | 15 | 16 | 17) || (0xC000..=0xFFFF).contains(&id)
}

fn restorable_format(id: u32) -> bool {
    if id < 0xC000 {
        return byte_format(id) || matches!(id, 2 | 3 | 9 | 14);
    }
    let mut name = [0u16; 256];
    let count = unsafe { GetClipboardFormatNameW(id, &mut name) };
    if count <= 0 {
        return false;
    }
    let name = String::from_utf16_lossy(&name[..count as usize]);
    // These OLE transfer formats can contain marshalled interfaces, process identity,
    // or storage/stream state; a byte copy cannot re-establish their ownership.
    ![
        "DataObject",
        "Ole Private Data",
        "Embed Source",
        "Embedded Object",
        "Link Source",
    ]
    .iter()
    .any(|opaque| name.eq_ignore_ascii_case(opaque))
}

fn duplicate_format(
    id: u32,
    source: HANDLE,
    remaining: usize,
) -> Result<(Format, usize), SelectionError> {
    if byte_format(id) {
        let (memory, size) = duplicate_memory(source, remaining)?;
        let handle = HANDLE(memory.0.0);
        std::mem::forget(memory);
        return Ok((Format { id, handle }, size));
    }
    unsafe {
        let bytes = match id {
            2 => {
                let mut bitmap = BITMAP::default();
                if GetObjectW(
                    HGDIOBJ(source.0),
                    size_of::<BITMAP>() as i32,
                    Some(&mut bitmap as *mut BITMAP as *mut _),
                ) == 0
                {
                    return Err(api_error(
                        "GetObjectW bitmap",
                        windows::core::Error::from_thread(),
                    ));
                }
                (bitmap.bmWidthBytes.unsigned_abs() as usize)
                    .saturating_mul(bitmap.bmHeight.unsigned_abs() as usize)
                    .saturating_mul(usize::from(bitmap.bmPlanes))
            }
            9 => {
                GetPaletteEntries(HPALETTE(source.0), 0, None) as usize * size_of::<PALETTEENTRY>()
            }
            14 => GetEnhMetaFileBits(HENHMETAFILE(source.0), None) as usize,
            3 => {
                let memory = HGLOBAL(source.0);
                if GlobalSize(memory) < size_of::<METAFILEPICT>() {
                    return Err(SelectionError::new(
                        ErrorKind::MalformedData,
                        "metafile picture",
                    ));
                }
                let picture = GlobalLock(memory).cast::<METAFILEPICT>();
                if picture.is_null() {
                    return Err(api_error(
                        "GlobalLock metafile",
                        windows::core::Error::from_thread(),
                    ));
                }
                let size = GetMetaFileBitsEx((*picture).hMF, 0, None) as usize;
                let _ = GlobalUnlock(memory);
                size.saturating_add(size_of::<METAFILEPICT>())
            }
            _ => {
                return Err(SelectionError::new(
                    ErrorKind::MalformedData,
                    "unsupported clipboard format",
                ));
            }
        };
        if bytes == 0 || bytes > remaining {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "clipboard graphics snapshot",
            ));
        }
        let handle = if id == 14 {
            HANDLE(CopyEnhMetaFileW(HENHMETAFILE(source.0), windows::core::PCWSTR::null()).0)
        } else {
            OleDuplicateData(source, CLIPBOARD_FORMAT(id as u16), GMEM_MOVEABLE)
        };
        if handle.is_invalid() {
            return Err(api_error(
                "duplicate clipboard graphics",
                windows::core::Error::from_thread(),
            ));
        }
        Ok((Format { id, handle }, bytes))
    }
}

impl ClipboardIo for NativeClipboard {
    type Saved = Vec<Format>;

    fn snapshot(&mut self, context: &Context) -> Result<Snapshot<Self::Saved>, SelectionError> {
        let _open = self.open(context.stage_deadline(Duration::from_millis(200)))?;
        let mut complete = true;
        let mut data = Vec::new();
        let mut size = 0;
        let mut format = 0;
        let mut count = 0;
        loop {
            context.check()?;
            unsafe {
                SetLastError(WIN32_ERROR(0));
            }
            format = unsafe { EnumClipboardFormats(format) };
            if format == 0 {
                if unsafe { GetLastError() }.0 != 0 {
                    complete = false;
                }
                break;
            }
            count += 1;
            if count > MAX_FORMATS {
                complete = false;
                break;
            }
            if !restorable_format(format) {
                complete = false;
                continue;
            }
            // GetClipboardData may invoke a delayed renderer. The caller's deadline still
            // expires independently; this fixed worker remains busy until the call returns.
            let saved = unsafe { GetClipboardData(format) }
                .map_err(|e| api_error("snapshot GetClipboardData", e))
                .and_then(|handle| duplicate_format(format, handle, MAX_SNAPSHOT - size));
            match saved {
                Ok((saved, bytes)) => {
                    size += bytes;
                    data.push(saved);
                }
                Err(_) => complete = false,
            }
        }
        // Delayed rendering and synthesized formats can advance the sequence while reading.
        let sequence = unsafe { GetClipboardSequenceNumber() };
        if sequence == 0 {
            complete = false;
        }
        Ok(Snapshot {
            data,
            sequence,
            complete,
        })
    }

    fn prepare_copy(&mut self, context: &Context, sequence: u32) -> Result<(), SelectionError> {
        loop {
            validate(context)?;
            let held = [
                VK_SHIFT,
                VK_CONTROL,
                VK_MENU,
                VK_LWIN,
                VK_RWIN,
                VIRTUAL_KEY(b'C' as u16),
            ]
            .iter()
            .any(|key| unsafe { GetAsyncKeyState(i32::from(key.0)) } < 0);
            if !held {
                break;
            }
            self.pause(context.state.deadline);
        }
        check_copy_allowed(context)?;
        validate(context)?;
        if sequence == 0 || unsafe { GetClipboardSequenceNumber() } != sequence {
            return Err(SelectionError::new(
                ErrorKind::ClipboardAmbiguous,
                "clipboard changed before Copy",
            ));
        }
        Ok(())
    }

    fn inject(&mut self) -> Result<(), SelectionError> {
        fn key(key: VIRTUAL_KEY, up: bool) -> INPUT {
            INPUT {
                r#type: INPUT_KEYBOARD,
                Anonymous: INPUT_0 {
                    ki: KEYBDINPUT {
                        wVk: key,
                        dwFlags: if up {
                            KEYEVENTF_KEYUP
                        } else {
                            KEYBD_EVENT_FLAGS::default()
                        },
                        ..Default::default()
                    },
                },
            }
        }
        let c = VIRTUAL_KEY(b'C' as u16);
        let events = [
            key(VK_CONTROL, false),
            key(c, false),
            key(c, true),
            key(VK_CONTROL, true),
        ];
        unsafe {
            SetLastError(WIN32_ERROR(0));
        }
        let sent = unsafe { SendInput(&events, size_of::<INPUT>() as i32) };
        if sent != events.len() as u32 {
            let code = unsafe { GetLastError() }.0 as i32;
            // Release only keys whose synthetic key-down was inserted. Never resend Copy.
            let mut releases = Vec::new();
            if sent == 2 {
                releases.push(key(c, true));
            }
            if (1..=3).contains(&sent) {
                releases.push(key(VK_CONTROL, true));
            }
            if !releases.is_empty() {
                unsafe {
                    SendInput(&releases, size_of::<INPUT>() as i32);
                }
            }
            return Err(SelectionError {
                kind: ErrorKind::InputInjectionFailed,
                operation: "SendInput",
                native_code: Some(code),
                clipboard_status: ClipboardStatus::Unknown,
            });
        }
        Ok(())
    }

    fn read_copy(
        &mut self,
        context: &Context,
        old_sequence: u32,
    ) -> Result<CopiedText, SelectionError> {
        // Reserve a small part of the total deadline for conditional restoration.
        let deadline = context
            .state
            .deadline
            .checked_sub(Duration::from_millis(100))
            .unwrap_or(context.state.deadline);
        loop {
            validate(context)?;
            if Instant::now() >= deadline {
                return Err(SelectionError::new(ErrorKind::TimedOut, "Copy response"));
            }
            self.pump();
            if unsafe { GetClipboardSequenceNumber() } == old_sequence {
                self.pause(deadline);
                continue;
            }
            let _open = self.open(deadline)?;
            let sequence = unsafe { GetClipboardSequenceNumber() };
            if sequence == old_sequence {
                continue;
            }
            if !self.attributable(context) {
                return Err(SelectionError::new(
                    ErrorKind::ClipboardAmbiguous,
                    "clipboard owner",
                ));
            }
            let text = (|| {
                let handle = unsafe { GetClipboardData(UNICODE_TEXT) }
                    .map_err(|e| api_error("GetClipboardData CF_UNICODETEXT", e))?;
                unsafe {
                    let memory = HGLOBAL(handle.0);
                    let size = GlobalSize(memory);
                    if size == 0
                        || size
                            > context
                                .options
                                .max_text_bytes
                                .saturating_mul(2)
                                .saturating_add(2)
                    {
                        return Err(SelectionError::new(
                            ErrorKind::LimitExceeded,
                            "clipboard text allocation",
                        ));
                    }
                    let pointer = GlobalLock(memory);
                    if pointer.is_null() {
                        return Err(api_error(
                            "GlobalLock text",
                            windows::core::Error::from_thread(),
                        ));
                    }
                    let text = clipboard_utf16(
                        std::slice::from_raw_parts(pointer.cast::<u8>(), size),
                        context.options.max_text_bytes,
                    );
                    let _ = GlobalUnlock(memory);
                    text
                }
            })();
            // Delayed Unicode rendering can increment the sequence while the clipboard is open.
            let sequence = unsafe { GetClipboardSequenceNumber() };
            return Ok(CopiedText { sequence, text });
        }
    }

    fn restore(
        &mut self,
        context: &Context,
        snapshot: Snapshot<Self::Saved>,
        sequence: u32,
    ) -> ClipboardStatus {
        if Instant::now() >= context.state.deadline {
            return ClipboardStatus::RestorationFailed;
        }
        let Ok(_open) = self.open(context.state.deadline) else {
            return ClipboardStatus::RestorationFailed;
        };
        if let Some(status) = restoration_decision(snapshot.complete, sequence, unsafe {
            GetClipboardSequenceNumber()
        }) {
            return status;
        }
        unsafe {
            if EmptyClipboard().is_err() {
                return ClipboardStatus::RestorationFailed;
            }
            let mut failed = false;
            for format in snapshot.data {
                if SetClipboardData(format.id, Some(format.handle)).is_ok() {
                    // Windows owns the allocation only after successful SetClipboardData.
                    std::mem::forget(format);
                } else {
                    failed = true;
                }
            }
            if failed {
                ClipboardStatus::RestorationFailed
            } else {
                ClipboardStatus::Restored
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::fixture::EditFixture;
    use super::*;

    #[test]
    fn opaque_ole_formats_are_not_restored_as_byte_allocations() {
        unsafe {
            let opaque = RegisterClipboardFormatW(w!("DataObject"));
            let html = RegisterClipboardFormatW(w!("HTML Format"));
            assert!(opaque >= 0xC000 && html >= 0xC000);
            assert!(!restorable_format(opaque));
            assert!(restorable_format(html));
            assert!(!restorable_format(0x0200)); // CF_PRIVATEFIRST
            assert!(!restorable_format(0x0080)); // CF_OWNERDISPLAY
        }
    }

    #[test]
    fn duplicated_clipboard_memory_is_independent_and_bounded() {
        unsafe {
            let memory = Memory(GlobalAlloc(GMEM_MOVEABLE, 8).unwrap());
            let pointer = GlobalLock(memory.0).cast::<u8>();
            assert!(!pointer.is_null());
            std::ptr::copy_nonoverlapping(b"original".as_ptr(), pointer, 8);
            let _ = GlobalUnlock(memory.0);
            let (copy, size) = duplicate_format(13, HANDLE(memory.0.0), 8).unwrap();
            assert_eq!(size, 8);
            assert!(duplicate_format(13, HANDLE(memory.0.0), 7).is_err());
            drop(memory);
            let pointer = GlobalLock(HGLOBAL(copy.handle.0)).cast::<u8>();
            assert_eq!(std::slice::from_raw_parts(pointer, 8), b"original");
            let _ = GlobalUnlock(HGLOBAL(copy.handle.0));
        }
    }

    #[test]
    fn duplicated_bitmap_uses_gdi_ownership_instead_of_global_memory() {
        unsafe {
            let bitmap = CreateBitmap(8, 8, 1, 32, None);
            assert!(!bitmap.is_invalid());
            let source = Format {
                id: 2,
                handle: HANDLE(bitmap.0),
            };
            let (copy, size) = duplicate_format(2, source.handle, 1024).unwrap();
            assert!(size > 0 && size <= 1024);
            assert!(duplicate_format(2, source.handle, 1).is_err());
            drop(source);
            let mut metadata = BITMAP::default();
            assert_ne!(
                GetObjectW(
                    HGDIOBJ(copy.handle.0),
                    size_of::<BITMAP>() as i32,
                    Some(&mut metadata as *mut BITMAP as *mut _)
                ),
                0
            );
            assert_eq!(metadata.bmWidth, 8);
        }
    }

    #[test]
    #[ignore = "activates a fixture and synthesizes Copy; select explicitly with --test-threads=1"]
    fn foreground_copy_restores_existing_clipboard() {
        let fixture = EditFixture::new("before 中文😀 after", 7, 11, false);
        let mut clipboard = NativeClipboard::new().unwrap();
        let original = clipboard.snapshot(&fixture.context()).unwrap();
        assert!(
            original.complete,
            "interactive fixture requires a completely preservable clipboard"
        );
        let old_foreground = unsafe { GetForegroundWindow() };
        struct RestoreForeground(HWND);
        impl Drop for RestoreForeground {
            fn drop(&mut self) {
                unsafe {
                    let _ = SetForegroundWindow(self.0);
                }
            }
        }
        let _foreground = RestoreForeground(old_foreground);
        unsafe {
            let _ = ShowWindow(hwnd(fixture.window), SW_SHOWNOACTIVATE);
            assert!(SetForegroundWindow(hwnd(fixture.window)).as_bool());
        }
        // Cross-thread activation is asynchronous. Wait for the fixture's message
        // loop to process it before starting the foreground snapshot transaction.
        let activation_deadline = Instant::now() + Duration::from_secs(2);
        while unsafe { GetForegroundWindow() } != hwnd(fixture.window)
            && Instant::now() < activation_deadline
        {
            std::thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(unsafe { GetForegroundWindow() }, hwnd(fixture.window));
        let mut context = fixture.context();
        context.source = super::super::capture_source().unwrap();
        let SelectionOutcome::Selected(text) = transaction(&mut clipboard, &context).unwrap()
        else {
            panic!("missing copied text")
        };
        assert_eq!(text.text, "中文😀");
        assert_eq!(text.clipboard_status, ClipboardStatus::Restored);
    }
}

use std::fmt;
use std::time::Duration;

/// Acquisition policy. Explicit strategies never attempt another method.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum CaptureStrategy {
    #[default]
    Auto = 0,
    Uia = 1,
    NativeEdit = 2,
    Clipboard = 3,
}

/// Per-request strategy and limits. Exclusions affect all acquisition methods.
#[derive(Clone, Debug)]
pub struct CaptureOptions {
    pub timeout: Duration,
    pub strategy: CaptureStrategy,
    /// Enable clipboard fallback in Auto mode; ignored by explicit strategies.
    pub copy_fallback: bool,
    pub max_text_bytes: usize,
    /// Case-insensitive executable basenames, for example `example.exe`.
    pub excluded_executables: Vec<String>,
    /// Native HWND values, valid only for the current process session.
    pub excluded_windows: Vec<usize>,
}

impl Default for CaptureOptions {
    fn default() -> Self {
        Self {
            timeout: Duration::from_secs(2),
            strategy: CaptureStrategy::Auto,
            copy_fallback: true,
            max_text_bytes: 1024 * 1024,
            excluded_executables: Vec::new(),
            excluded_windows: Vec::new(),
        }
    }
}

impl CaptureOptions {
    /// Validate resource limits and exclusion names without accessing Windows.
    pub fn validate(&self) -> Result<(), SelectionError> {
        if self.timeout.is_zero()
            || self.timeout > Duration::from_secs(60)
            || self.max_text_bytes == 0
            || self.max_text_bytes > 64 * 1024 * 1024
            || self.excluded_windows.len() > 1024
            || self.excluded_executables.len() > 1024
            || self.excluded_executables.iter().any(|name| {
                name.is_empty() || name.len() > 1024 || name.contains(['\0', '/', '\\'])
            })
        {
            return Err(SelectionError::new(
                ErrorKind::InvalidConfiguration,
                "options",
            ));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceWindow {
    pub window: usize,
    pub process_id: u32,
    pub focused_control: usize,
    pub executable: String,
}

/// Physical screen coordinates. Geometry is optional and never required for text success.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SelectionRect {
    pub left: f64,
    pub top: f64,
    pub width: f64,
    pub height: f64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SelectedRange {
    pub text: String,
    pub bounds: Vec<SelectionRect>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum RetrievalMethod {
    Uia = 1,
    NativeEdit = 2,
    Clipboard = 3,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u32)]
pub enum ClipboardStatus {
    #[default]
    Unchanged = 0,
    Restored = 1,
    PreservationIncomplete = 2,
    Superseded = 3,
    RestorationFailed = 4,
    /// The caller stopped waiting after clipboard work began. Cleanup may still finish.
    Unknown = 5,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SelectedText {
    pub text: String,
    pub ranges: Vec<SelectedRange>,
    pub source: SourceWindow,
    pub method: RetrievalMethod,
    pub clipboard_status: ClipboardStatus,
}

#[derive(Clone, Debug, PartialEq)]
pub enum SelectionOutcome {
    Selected(SelectedText),
    NoSelection,
    Unsupported,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ErrorKind {
    InvalidConfiguration = 1,
    Busy = 2,
    WorkerUnavailable = 3,
    TimedOut = 4,
    Cancelled = 5,
    NoForegroundWindow = 6,
    TargetChanged = 7,
    ProtectedContent = 8,
    AccessDenied = 9,
    ClipboardBusy = 10,
    ClipboardAmbiguous = 11,
    InputInjectionFailed = 12,
    MalformedData = 13,
    LimitExceeded = 14,
    UnsupportedPlatform = 15,
    NativeApi = 16,
    CopyBlocked = 17,
    TargetUnavailable = 18,
}

/// Machine-readable failure. Display contains operation names, never captured text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SelectionError {
    pub kind: ErrorKind,
    pub operation: &'static str,
    /// HRESULT bit pattern or Win32 error, depending on the named operation.
    pub native_code: Option<i32>,
    pub clipboard_status: ClipboardStatus,
}

impl SelectionError {
    pub(crate) fn new(kind: ErrorKind, operation: &'static str) -> Self {
        Self {
            kind,
            operation,
            native_code: None,
            clipboard_status: ClipboardStatus::Unchanged,
        }
    }
}

impl fmt::Display for SelectionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?} during {}", self.kind, self.operation)?;
        if let Some(code) = self.native_code {
            write!(f, " (0x{:08X})", code as u32)?;
        }
        Ok(())
    }
}

impl std::error::Error for SelectionError {}

pub type CaptureResult = Result<SelectionOutcome, SelectionError>;

#[cfg(any(windows, test))]
pub(crate) fn assemble(
    ranges: Vec<SelectedRange>,
    source: SourceWindow,
    method: RetrievalMethod,
    clipboard_status: ClipboardStatus,
    limit: usize,
) -> CaptureResult {
    let ranges: Vec<_> = ranges
        .into_iter()
        .filter(|range| !range.text.is_empty())
        .collect();
    if ranges.is_empty() {
        return Ok(SelectionOutcome::NoSelection);
    }
    let length = ranges.iter().try_fold(ranges.len() - 1, |sum, range| {
        sum.checked_add(range.text.len())
    });
    if ranges.len() > 128 || length.is_none_or(|length| length > limit) {
        let mut error = SelectionError::new(ErrorKind::LimitExceeded, "selected text");
        error.clipboard_status = clipboard_status;
        return Err(error);
    }
    let text = ranges
        .iter()
        .map(|range| range.text.as_str())
        .collect::<Vec<_>>()
        .join("\n");
    Ok(SelectionOutcome::Selected(SelectedText {
        text,
        ranges,
        source,
        method,
        clipboard_status,
    }))
}

#[cfg(any(windows, test))]
pub(crate) fn decode_utf16(units: &[u16], limit: usize) -> Result<String, SelectionError> {
    // A valid UTF-8 encoding cannot be shorter in bytes than its UTF-16 unit count.
    // Enforce this before allocating, including providers that ignore GetText's limit.
    if units.len() > limit {
        return Err(SelectionError::new(ErrorKind::LimitExceeded, "UTF-16"));
    }
    let text = String::from_utf16(units)
        .map_err(|_| SelectionError::new(ErrorKind::MalformedData, "UTF-16"))?;
    if text.len() > limit {
        return Err(SelectionError::new(ErrorKind::LimitExceeded, "UTF-16"));
    }
    Ok(text)
}

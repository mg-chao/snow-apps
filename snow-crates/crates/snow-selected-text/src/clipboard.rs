//! Clipboard transaction policy, independent of Win32 handles and memory allocation.
use std::sync::atomic::Ordering;

use crate::model::*;
use crate::policy::Context;

pub(crate) struct Snapshot<T> {
    pub data: T,
    pub sequence: u32,
    pub complete: bool,
}

pub(crate) struct CopiedText {
    pub sequence: u32,
    pub text: Result<String, SelectionError>,
}

pub(crate) trait ClipboardIo {
    type Saved;
    fn snapshot(&mut self, context: &Context) -> Result<Snapshot<Self::Saved>, SelectionError>;
    fn prepare_copy(&mut self, context: &Context, sequence: u32) -> Result<(), SelectionError>;
    fn inject(&mut self) -> Result<(), SelectionError>;
    fn read_copy(
        &mut self,
        context: &Context,
        old_sequence: u32,
    ) -> Result<CopiedText, SelectionError>;
    fn restore(
        &mut self,
        context: &Context,
        snapshot: Snapshot<Self::Saved>,
        copied_sequence: u32,
    ) -> ClipboardStatus;
}

pub(crate) fn transaction(io: &mut impl ClipboardIo, context: &Context) -> CaptureResult {
    context.check()?;
    let snapshot = io.snapshot(context)?;
    context.check()?;
    io.prepare_copy(context, snapshot.sequence)?;
    context.check()?;
    context.state.copy_started.store(true, Ordering::Release);
    io.inject().map_err(|mut error| {
        error.clipboard_status = ClipboardStatus::Unknown;
        error
    })?;
    let copied = io
        .read_copy(context, snapshot.sequence)
        .map_err(|mut error| {
            // No attributable copy was observed. Never restore over an unknown/newer writer.
            error.clipboard_status = ClipboardStatus::Unknown;
            error
        })?;
    let status = io.restore(context, snapshot, copied.sequence);
    let text = context
        .check()
        .and(copied.text)
        .and_then(|text| {
            if text.is_empty() {
                Err(SelectionError::new(
                    ErrorKind::ClipboardAmbiguous,
                    "Copy returned empty text",
                ))
            } else {
                Ok(text)
            }
        })
        .map_err(|mut error| {
            error.clipboard_status = status;
            error
        })?;
    assemble(
        vec![SelectedRange {
            text,
            bounds: Vec::new(),
        }],
        context.source.clone(),
        RetrievalMethod::Clipboard,
        status,
        context.options.max_text_bytes,
    )
}

pub(crate) fn restoration_decision(
    complete: bool,
    expected: u32,
    current: u32,
) -> Option<ClipboardStatus> {
    if expected != current {
        Some(ClipboardStatus::Superseded)
    } else if !complete {
        Some(ClipboardStatus::PreservationIncomplete)
    } else {
        None
    }
}

pub(crate) fn clipboard_utf16(bytes: &[u8], limit: usize) -> Result<String, SelectionError> {
    if !bytes.len().is_multiple_of(2) {
        return Err(SelectionError::new(
            ErrorKind::MalformedData,
            "CF_UNICODETEXT size",
        ));
    }
    // The allocation may include padding beyond the terminating NUL.
    let mut units = Vec::new();
    for pair in bytes.chunks_exact(2) {
        let unit = u16::from_le_bytes([pair[0], pair[1]]);
        if unit == 0 {
            return decode_utf16(&units, limit);
        }
        if units.len() >= limit {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "CF_UNICODETEXT",
            ));
        }
        units.push(unit);
    }
    Err(SelectionError::new(
        ErrorKind::MalformedData,
        "CF_UNICODETEXT termination",
    ))
}

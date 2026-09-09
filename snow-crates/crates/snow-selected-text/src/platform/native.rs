use std::time::Instant;

use windows::Win32::Foundation::{GetLastError, HWND, LPARAM, SetLastError, WIN32_ERROR, WPARAM};
use windows::Win32::UI::Controls::EM_GETSEL;
use windows::Win32::UI::WindowsAndMessaging::*;

use super::{class_name, hwnd, validate};
use crate::model::*;
use crate::policy::{Context, Probe};

pub(super) fn check_password(window: HWND) -> Result<(), SelectionError> {
    let class = class_name(window).to_ascii_lowercase();
    if (class == "edit" || class.starts_with("richedit"))
        && unsafe { GetWindowLongPtrW(window, GWL_STYLE) } & ES_PASSWORD as isize != 0
    {
        return Err(SelectionError::new(
            ErrorKind::ProtectedContent,
            "password edit control",
        ));
    }
    Ok(())
}

fn send(
    window: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    deadline: Instant,
) -> Result<usize, SelectionError> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return Err(SelectionError::new(
            ErrorKind::TimedOut,
            "native edit query",
        ));
    }
    let mut result = 0;
    unsafe {
        SetLastError(WIN32_ERROR(0));
        if SendMessageTimeoutW(
            window,
            message,
            wparam,
            lparam,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
            remaining.as_millis().clamp(1, u128::from(u32::MAX)) as u32,
            Some(&mut result),
        )
        .0 == 0
        {
            let code = GetLastError().0;
            return Err(SelectionError {
                kind: if code == 5 {
                    ErrorKind::AccessDenied
                } else if code == 0 || code == 1460 {
                    ErrorKind::TimedOut
                } else {
                    ErrorKind::NativeApi
                },
                operation: "SendMessageTimeoutW",
                native_code: Some(code as i32),
                clipboard_status: ClipboardStatus::Unchanged,
            });
        }
    }
    Ok(result)
}

fn selection(window: HWND, deadline: Instant) -> Result<(u32, u32), SelectionError> {
    let (mut start, mut end) = (0u32, 0u32);
    // EM_GETSEL is a system message: Windows marshals these DWORD pointers across processes.
    send(
        window,
        EM_GETSEL,
        WPARAM(&mut start as *mut u32 as usize),
        LPARAM(&mut end as *mut u32 as isize),
        deadline,
    )?;
    Ok((start, end))
}

pub(super) fn capture(context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
    validate(context)?;
    let result = read_edit(context, deadline)?;
    validate(context)?;
    Ok(result)
}

fn read_edit(context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
    let window = hwnd(context.source.focused_control);
    check_password(window)?;
    if !class_name(window).eq_ignore_ascii_case("edit") {
        return Ok(Probe::Unsupported);
    }
    let (start, end) = selection(window, deadline)?;
    if start == end {
        return Ok(Probe::Empty);
    }
    let length = send(window, WM_GETTEXTLENGTH, WPARAM(0), LPARAM(0), deadline)?;
    // Bound the whole-control buffer as well as the selected substring.
    if length > 32 * 1024 * 1024 {
        return Err(SelectionError::new(
            ErrorKind::LimitExceeded,
            "edit document",
        ));
    }
    let mut buffer = vec![0u16; length + 1];
    context.check()?;
    let copied = send(
        window,
        WM_GETTEXT,
        WPARAM(buffer.len()),
        LPARAM(buffer.as_mut_ptr() as isize),
        deadline,
    )?;
    if copied >= buffer.len() || start > end || end as usize > copied {
        return Err(SelectionError::new(
            ErrorKind::TargetChanged,
            "edit selection bounds",
        ));
    }
    context.check()?;
    if selection(window, deadline)? != (start, end) {
        return Err(SelectionError::new(
            ErrorKind::TargetChanged,
            "edit selection changed",
        ));
    }
    let text = decode_utf16(
        &buffer[start as usize..end as usize],
        context.options.max_text_bytes,
    )?;
    Ok(Probe::Selected(vec![SelectedRange {
        text,
        bounds: Vec::new(),
    }]))
}

#[cfg(test)]
mod tests {
    use super::super::fixture::EditFixture;
    use super::*;
    use std::time::Duration;

    #[test]
    fn hidden_native_edit_reads_unicode_and_full_32bit_selection_offsets() {
        let prefix = "x".repeat(70000);
        let fixture = EditFixture::new(&format!("{prefix}中文😀 tail"), 70000, 70004, false);
        let Probe::Selected(ranges) =
            read_edit(&fixture.context(), Instant::now() + Duration::from_secs(2)).unwrap()
        else {
            panic!("missing native selection")
        };
        assert_eq!(ranges[0].text, "中文😀");
    }

    #[test]
    fn hidden_native_edit_distinguishes_caret_password_and_oversized_selection() {
        let fixture = EditFixture::new("abcdef", 2, 2, false);
        assert!(matches!(
            read_edit(&fixture.context(), Instant::now() + Duration::from_secs(2)),
            Ok(Probe::Empty)
        ));
        let fixture = EditFixture::new("secret", 0, 6, true);
        assert_eq!(
            read_edit(&fixture.context(), Instant::now() + Duration::from_secs(2))
                .unwrap_err()
                .kind,
            ErrorKind::ProtectedContent
        );
        let fixture = EditFixture::new("abcdef", 0, 6, false);
        let mut context = fixture.context();
        context.options.max_text_bytes = 5;
        assert_eq!(
            read_edit(&context, Instant::now() + Duration::from_secs(2))
                .unwrap_err()
                .kind,
            ErrorKind::LimitExceeded
        );
    }

    #[test]
    fn expired_native_budget_sends_no_message() {
        assert_eq!(
            send(hwnd(0), WM_GETTEXT, WPARAM(0), LPARAM(0), Instant::now())
                .unwrap_err()
                .kind,
            ErrorKind::TimedOut
        );
    }
}

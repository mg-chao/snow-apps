use std::collections::VecDeque;
use std::time::Instant;

use windows::Win32::Foundation::{E_NOINTERFACE, E_POINTER};
use windows::Win32::System::Com::{CLSCTX_INPROC_SERVER, CoCreateInstance, SAFEARRAY};
use windows::Win32::System::Ole::*;
use windows::Win32::UI::Accessibility::*;
use windows::Win32::UI::WindowsAndMessaging::{GA_ROOT, GetAncestor};

use super::{api_error, hwnd, validate};
use crate::model::*;
use crate::policy::{Context, Probe};

pub(super) struct Automation(IUIAutomation2);

impl Automation {
    pub fn new() -> Result<Self, SelectionError> {
        unsafe { CoCreateInstance(&CUIAutomation8, None, CLSCTX_INPROC_SERVER) }
            .map(Self)
            .map_err(|e| api_error("CUIAutomation8", e))
    }

    fn budget(&self, context: &Context, deadline: Instant) -> Result<(), SelectionError> {
        context.check()?;
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(SelectionError::new(ErrorKind::TimedOut, "UIA budget"));
        }
        let millis = remaining.as_millis().clamp(1, u128::from(u32::MAX)) as u32;
        unsafe {
            self.0
                .SetConnectionTimeout(millis)
                .map_err(|e| api_error("UIA connection timeout", e))?;
            self.0
                .SetTransactionTimeout(millis)
                .map_err(|e| api_error("UIA transaction timeout", e))?;
        }
        Ok(())
    }

    pub fn capture(&self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
        let result = self.capture_once(context, deadline);
        match result {
            Err(error) if error.kind == ErrorKind::TargetUnavailable => {
                validate(context)?;
                self.budget(context, deadline)?;
                self.capture_once(context, deadline)
            }
            result => result,
        }
    }

    fn capture_once(&self, context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
        self.budget(context, deadline)?;
        let focus =
            unsafe { self.0.GetFocusedElement() }.map_err(|e| api_error("GetFocusedElement", e))?;
        let walker =
            unsafe { self.0.ControlViewWalker() }.map_err(|e| api_error("ControlViewWalker", e))?;
        // Establish ancestry before trusting any selection, including multi-process browser content.
        let mut ancestors = Vec::new();
        let mut current = Some(focus.clone());
        let mut belongs = false;
        for _ in 0..16 {
            let Some(element) = current.take() else {
                break;
            };
            self.budget(context, deadline)?;
            let native = unsafe { element.CurrentNativeWindowHandle() }
                .map_err(|e| api_error("UIA native window", e))?;
            if !native.is_invalid()
                && (native == hwnd(context.source.window)
                    || unsafe { GetAncestor(native, GA_ROOT) } == hwnd(context.source.window))
            {
                belongs = true;
            }
            let at_root = native == hwnd(context.source.window);
            ancestors.push(element.clone());
            if at_root {
                break;
            }
            self.budget(context, deadline)?;
            current = nullable(unsafe { walker.GetParentElement(&element) }, "UIA parent")?;
        }
        if !belongs {
            return Err(SelectionError::new(
                ErrorKind::TargetChanged,
                "UIA focus ancestry",
            ));
        }
        for element in &ancestors {
            let result = self.probe(element, context, deadline)?;
            if !matches!(result, Probe::Unsupported) {
                self.check_focus(&focus, context, deadline)?;
                return Ok(result);
            }
        }
        // Search only the focused container, never siblings of a focused leaf or the desktop.
        self.budget(context, deadline)?;
        let kind =
            unsafe { focus.CurrentControlType() }.map_err(|e| api_error("UIA control type", e))?;
        if kind == UIA_PaneControlTypeId
            || kind == UIA_DocumentControlTypeId
            || kind == UIA_WindowControlTypeId
        {
            let mut queue = VecDeque::from([(focus.clone(), 0usize)]);
            let mut documents = Vec::new();
            let mut visited = 0;
            while let Some((parent, depth)) = queue.pop_front() {
                self.budget(context, deadline)?;
                let mut child =
                    nullable(unsafe { walker.GetFirstChildElement(&parent) }, "UIA child")?;
                while let Some(element) = child {
                    visited += 1;
                    if visited > 128 {
                        return Err(SelectionError::new(
                            ErrorKind::LimitExceeded,
                            "UIA traversal",
                        ));
                    }
                    self.budget(context, deadline)?;
                    let kind = unsafe { element.CurrentControlType() }
                        .map_err(|e| api_error("UIA control type", e))?;
                    if kind == UIA_DocumentControlTypeId {
                        documents.push(element.clone());
                        if documents.len() > 1 {
                            return Ok(Probe::Unsupported);
                        }
                    } else if depth < 7 {
                        queue.push_back((element.clone(), depth + 1));
                    }
                    self.budget(context, deadline)?;
                    child = nullable(
                        unsafe { walker.GetNextSiblingElement(&element) },
                        "UIA sibling",
                    )?;
                }
            }
            if let Some(document) = documents.first() {
                let probe = self.probe(document, context, deadline)?;
                self.check_focus(&focus, context, deadline)?;
                return Ok(probe);
            }
        }
        self.check_focus(&focus, context, deadline)?;
        Ok(Probe::Unsupported)
    }

    fn check_focus(
        &self,
        focus: &IUIAutomationElement,
        context: &Context,
        deadline: Instant,
    ) -> Result<(), SelectionError> {
        validate(context)?;
        self.budget(context, deadline)?;
        let now =
            unsafe { self.0.GetFocusedElement() }.map_err(|e| api_error("GetFocusedElement", e))?;
        self.budget(context, deadline)?;
        if !unsafe { self.0.CompareElements(focus, &now) }
            .map_err(|e| api_error("UIA focus comparison", e))?
            .as_bool()
        {
            return Err(SelectionError::new(ErrorKind::TargetChanged, "UIA focus"));
        }
        Ok(())
    }

    fn probe(
        &self,
        element: &IUIAutomationElement,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError> {
        self.budget(context, deadline)?;
        if unsafe { element.CurrentIsPassword() }
            .map_err(|e| api_error("UIA password property", e))?
            .as_bool()
        {
            return Err(SelectionError::new(
                ErrorKind::ProtectedContent,
                "UIA password field",
            ));
        }
        self.budget(context, deadline)?;
        let pattern: IUIAutomationTextPattern =
            match unsafe { element.GetCurrentPatternAs(UIA_TextPatternId) } {
                Ok(pattern) => pattern,
                Err(error) if unsupported(&error) => return Ok(Probe::Unsupported),
                Err(error) => return Err(api_error("UIA TextPattern", error)),
            };
        self.budget(context, deadline)?;
        if unsafe { pattern.SupportedTextSelection() }
            .map_err(|e| api_error("SupportedTextSelection", e))?
            == SupportedTextSelection_None
        {
            return Ok(Probe::Unsupported);
        }
        self.budget(context, deadline)?;
        let Some(selection) = nullable(unsafe { pattern.GetSelection() }, "GetSelection")? else {
            return Ok(Probe::Unsupported);
        };
        self.budget(context, deadline)?;
        let length = unsafe { selection.Length() }.map_err(|e| api_error("UIA range count", e))?;
        if !(0..=128).contains(&length) {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "UIA range count",
            ));
        }
        let mut ranges = Vec::new();
        let mut bytes = 0usize;
        for index in 0..length {
            self.budget(context, deadline)?;
            let range =
                unsafe { selection.GetElement(index) }.map_err(|e| api_error("UIA range", e))?;
            self.budget(context, deadline)?;
            // maxLength counts UTF-16 units. One extra detects an oversized response.
            let text = unsafe { range.GetText((context.options.max_text_bytes + 1) as i32) }
                .map_err(|e| api_error("UIA GetText", e))?;
            let text = decode_utf16(&text, context.options.max_text_bytes)?;
            if text.is_empty() {
                continue;
            }
            bytes = bytes
                .saturating_add(text.len())
                .saturating_add(usize::from(!ranges.is_empty()));
            if bytes > context.options.max_text_bytes {
                return Err(SelectionError::new(ErrorKind::LimitExceeded, "UIA text"));
            }
            self.budget(context, deadline)?;
            let bounds = unsafe { range.GetBoundingRectangles() }
                .ok()
                .map(rectangles)
                .unwrap_or_default();
            ranges.push(SelectedRange { text, bounds });
        }
        if ranges.is_empty() {
            Ok(Probe::Empty)
        } else {
            Ok(Probe::Selected(ranges))
        }
    }
}

fn unsupported(error: &windows::core::Error) -> bool {
    error.code() == E_NOINTERFACE
        || error.code() == E_POINTER
        || error.code().0 as u32 == UIA_E_NOTSUPPORTED
}

fn nullable<T>(
    value: windows::core::Result<T>,
    operation: &'static str,
) -> Result<Option<T>, SelectionError> {
    match value {
        Ok(value) => Ok(Some(value)),
        // windows-rs maps successful null interface outputs to E_POINTER.
        Err(error) if error.code() == E_POINTER => Ok(None),
        Err(error) => Err(api_error(operation, error)),
    }
}

struct Array(*mut SAFEARRAY);
impl Drop for Array {
    fn drop(&mut self) {
        unsafe {
            let _ = SafeArrayDestroy(self.0);
        }
    }
}

fn rectangles(raw: *mut SAFEARRAY) -> Vec<SelectionRect> {
    if raw.is_null() {
        return Vec::new();
    }
    let array = Array(raw);
    unsafe {
        if SafeArrayGetDim(array.0) != 1 || SafeArrayGetElemsize(array.0) != size_of::<f64>() as u32
        {
            return Vec::new();
        }
        let (Ok(low), Ok(high)) = (
            SafeArrayGetLBound(array.0, 1),
            SafeArrayGetUBound(array.0, 1),
        ) else {
            return Vec::new();
        };
        let count = i64::from(high) - i64::from(low) + 1;
        if count <= 0 || count > 4096 || count % 4 != 0 {
            return Vec::new();
        }
        let mut values = Vec::with_capacity(count as usize);
        for offset in 0..count {
            let index = (i64::from(low) + offset) as i32;
            let mut value = 0f64;
            if SafeArrayGetElement(array.0, &index, &mut value as *mut f64 as *mut _).is_err()
                || !value.is_finite()
            {
                return Vec::new();
            }
            values.push(value);
        }
        values
            .chunks_exact(4)
            .filter(|v| v[2] > 0.0 && v[3] > 0.0)
            .map(|v| SelectionRect {
                left: v[0],
                top: v[1],
                width: v[2],
                height: v[3],
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::super::{Apartment, fixture::EditFixture};
    use super::*;
    use std::time::Duration;

    // UIA's in-process proxy lifecycle is shared; do not tear down another test's
    // originating COM apartment while it is establishing an element connection.
    static UIA_FIXTURE_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn hidden_edit_uia_provider_reads_selection_and_reports_caret() {
        let _lock = UIA_FIXTURE_LOCK.lock().unwrap();
        let fixture = EditFixture::new("hello 中文😀 world", 6, 10, false);
        let _com = Apartment::new().unwrap();
        let automation = Automation::new().unwrap();
        let element = unsafe { automation.0.ElementFromHandle(hwnd(fixture.window)) }.unwrap();
        let probe = automation
            .probe(
                &element,
                &fixture.context(),
                Instant::now() + Duration::from_secs(3),
            )
            .unwrap();
        let Probe::Selected(ranges) = probe else {
            panic!("hidden edit did not expose TextPattern: {probe:?}")
        };
        assert_eq!(ranges[0].text, "中文😀");
        let fixture = EditFixture::new("hello", 2, 2, false);
        let element = unsafe { automation.0.ElementFromHandle(hwnd(fixture.window)) }.unwrap();
        assert!(matches!(
            automation
                .probe(
                    &element,
                    &fixture.context(),
                    Instant::now() + Duration::from_secs(3)
                )
                .unwrap(),
            Probe::Empty
        ));
    }

    #[test]
    fn hidden_edit_uia_password_is_never_read() {
        let _lock = UIA_FIXTURE_LOCK.lock().unwrap();
        let fixture = EditFixture::new("secret", 0, 6, true);
        let _com = Apartment::new().unwrap();
        let automation = Automation::new().unwrap();
        let element = unsafe { automation.0.ElementFromHandle(hwnd(fixture.window)) }.unwrap();
        assert_eq!(
            automation
                .probe(
                    &element,
                    &fixture.context(),
                    Instant::now() + Duration::from_secs(3)
                )
                .unwrap_err()
                .kind,
            ErrorKind::ProtectedContent
        );
    }

    #[test]
    fn uia_error_mapping_keeps_native_code_and_distinguishes_null_patterns() {
        assert!(unsupported(&windows::core::Error::from_hresult(
            E_NOINTERFACE
        )));
        assert!(
            nullable::<()>(Err(windows::core::Error::from_hresult(E_POINTER)), "test")
                .unwrap()
                .is_none()
        );
        for (code, kind) in [
            (0x80070005u32, ErrorKind::AccessDenied),
            (0x80131505u32, ErrorKind::TimedOut),
            (0x80040201u32, ErrorKind::TargetUnavailable),
        ] {
            let error = api_error(
                "test",
                windows::core::Error::from_hresult(windows::core::HRESULT(code as i32)),
            );
            assert_eq!(error.kind, kind);
            assert_eq!(error.native_code, Some(code as i32));
        }
    }
}

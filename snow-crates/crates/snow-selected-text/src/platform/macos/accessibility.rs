use std::ffi::c_void;
use std::time::Instant;

use accessibility_sys::*;
use core_foundation::array::CFArray;
use core_foundation::base::{Boolean, CFRange, CFType, TCFType};
use core_foundation::string::{CFString, CFStringGetBytes, kCFStringEncodingUTF8};

use super::validate;
use crate::model::*;
use crate::policy::{Context, Probe};

const MAX_RANGES: usize = 128;
const AX_ERROR_SUCCESS: AXError = kAXErrorSuccess;
const AX_ERROR_FAILURE: AXError = kAXErrorFailure;
const AX_ERROR_ILLEGAL_ARGUMENT: AXError = kAXErrorIllegalArgument;
const AX_ERROR_INVALID_UI_ELEMENT: AXError = kAXErrorInvalidUIElement;
const AX_ERROR_CANNOT_COMPLETE: AXError = kAXErrorCannotComplete;
const AX_ERROR_ATTRIBUTE_UNSUPPORTED: AXError = kAXErrorAttributeUnsupported;
const AX_ERROR_NOT_IMPLEMENTED: AXError = kAXErrorNotImplemented;
const AX_ERROR_API_DISABLED: AXError = kAXErrorAPIDisabled;
const AX_ERROR_NO_VALUE: AXError = kAXErrorNoValue;
const AX_ERROR_PARAMETERIZED_ATTRIBUTE_UNSUPPORTED: AXError =
    kAXErrorParameterizedAttributeUnsupported;

#[derive(Clone)]
struct Element(CFType);

impl Element {
    fn application(process_id: u32) -> Result<Self, SelectionError> {
        let raw = unsafe { AXUIElementCreateApplication(process_id as i32) };
        if raw.is_null() {
            return Err(SelectionError::new(
                ErrorKind::TargetUnavailable,
                "AXUIElementCreateApplication",
            ));
        }
        let value = unsafe { CFType::wrap_under_create_rule(raw.cast()) };
        Ok(Self(value))
    }

    fn from_value(value: CFType, operation: &'static str) -> Result<Self, SelectionError> {
        if value.type_of() != unsafe { AXUIElementGetTypeID() } {
            return Err(SelectionError::new(ErrorKind::MalformedData, operation));
        }
        Ok(Self(value))
    }

    fn raw(&self) -> AXUIElementRef {
        self.0.as_CFTypeRef().cast_mut().cast()
    }
}

fn error(operation: &'static str, code: AXError) -> SelectionError {
    let kind = match code {
        AX_ERROR_API_DISABLED => ErrorKind::AccessDenied,
        AX_ERROR_CANNOT_COMPLETE => ErrorKind::TimedOut,
        AX_ERROR_INVALID_UI_ELEMENT => ErrorKind::TargetUnavailable,
        AX_ERROR_ILLEGAL_ARGUMENT => ErrorKind::MalformedData,
        _ => ErrorKind::NativeApi,
    };
    SelectionError {
        kind,
        operation,
        native_code: Some(code),
        clipboard_status: ClipboardStatus::Unchanged,
    }
}

fn is_recoverable(error: &SelectionError) -> bool {
    matches!(
        error.native_code,
        Some(AX_ERROR_CANNOT_COMPLETE | AX_ERROR_FAILURE)
    ) || (error.kind == ErrorKind::TimedOut && error.operation == "Accessibility budget")
}

fn set_timeout(
    element: &Element,
    context: &Context,
    deadline: Instant,
) -> Result<(), SelectionError> {
    context.check()?;
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return Err(SelectionError::new(
            ErrorKind::TimedOut,
            "Accessibility budget",
        ));
    }
    let code = unsafe {
        AXUIElementSetMessagingTimeout(element.raw(), remaining.as_secs_f32().max(0.001))
    };
    if code == AX_ERROR_SUCCESS {
        Ok(())
    } else {
        Err(error("AXUIElementSetMessagingTimeout", code))
    }
}

fn copy_attribute(
    element: &Element,
    name: &'static str,
    context: &Context,
    deadline: Instant,
) -> Result<Option<CFType>, SelectionError> {
    set_timeout(element, context, deadline)?;
    let attribute = CFString::new(name);
    let mut value = std::ptr::null();
    let code = unsafe {
        AXUIElementCopyAttributeValue(element.raw(), attribute.as_concrete_TypeRef(), &mut value)
    };
    match code {
        AX_ERROR_SUCCESS if !value.is_null() => {
            Ok(Some(unsafe { CFType::wrap_under_create_rule(value) }))
        }
        AX_ERROR_SUCCESS => Err(SelectionError::new(ErrorKind::MalformedData, name)),
        AX_ERROR_ATTRIBUTE_UNSUPPORTED | AX_ERROR_NO_VALUE | AX_ERROR_NOT_IMPLEMENTED => Ok(None),
        _ => Err(error(name, code)),
    }
}

fn copy_parameterized(
    element: &Element,
    name: &'static str,
    parameter: &CFType,
    context: &Context,
    deadline: Instant,
) -> Result<Option<CFType>, SelectionError> {
    set_timeout(element, context, deadline)?;
    let attribute = CFString::new(name);
    let mut value = std::ptr::null();
    let code = unsafe {
        AXUIElementCopyParameterizedAttributeValue(
            element.raw(),
            attribute.as_concrete_TypeRef(),
            parameter.as_CFTypeRef(),
            &mut value,
        )
    };
    match code {
        AX_ERROR_SUCCESS if !value.is_null() => {
            Ok(Some(unsafe { CFType::wrap_under_create_rule(value) }))
        }
        AX_ERROR_SUCCESS => Err(SelectionError::new(ErrorKind::MalformedData, name)),
        AX_ERROR_ATTRIBUTE_UNSUPPORTED
        | AX_ERROR_PARAMETERIZED_ATTRIBUTE_UNSUPPORTED
        | AX_ERROR_NO_VALUE
        | AX_ERROR_NOT_IMPLEMENTED => Ok(None),
        _ => Err(error(name, code)),
    }
}

fn string(
    value: CFType,
    operation: &'static str,
    max_text_bytes: usize,
) -> Result<String, SelectionError> {
    let value = value
        .downcast_into::<CFString>()
        .ok_or_else(|| SelectionError::new(ErrorKind::MalformedData, operation))?;
    let char_length = value.char_len();
    let minimum_bytes = usize::try_from(char_length)
        .map_err(|_| SelectionError::new(ErrorKind::MalformedData, operation))?;
    if minimum_bytes > max_text_bytes {
        return Err(SelectionError::new(ErrorKind::LimitExceeded, operation));
    }
    let range = CFRange::init(0, char_length);
    let mut bytes_required = 0;
    let converted = unsafe {
        CFStringGetBytes(
            value.as_concrete_TypeRef(),
            range,
            kCFStringEncodingUTF8,
            0,
            false as Boolean,
            std::ptr::null_mut(),
            0,
            &mut bytes_required,
        )
    };
    let byte_length = usize::try_from(bytes_required)
        .map_err(|_| SelectionError::new(ErrorKind::MalformedData, operation))?;
    if converted != char_length {
        return Err(SelectionError::new(ErrorKind::MalformedData, operation));
    }
    if byte_length > max_text_bytes {
        return Err(SelectionError::new(ErrorKind::LimitExceeded, operation));
    }
    let mut buffer = vec![0; byte_length];
    let mut bytes_used = 0;
    let converted = unsafe {
        CFStringGetBytes(
            value.as_concrete_TypeRef(),
            range,
            kCFStringEncodingUTF8,
            0,
            false as Boolean,
            buffer.as_mut_ptr(),
            bytes_required,
            &mut bytes_used,
        )
    };
    if converted != char_length || bytes_used != bytes_required {
        return Err(SelectionError::new(ErrorKind::MalformedData, operation));
    }
    String::from_utf8(buffer).map_err(|_| SelectionError::new(ErrorKind::MalformedData, operation))
}

fn range(value: &CFType, operation: &'static str) -> Result<CFRange, SelectionError> {
    if value.type_of() != unsafe { AXValueGetTypeID() }
        || unsafe { AXValueGetType(value.as_CFTypeRef().cast_mut().cast()) } != kAXValueTypeCFRange
    {
        return Err(SelectionError::new(ErrorKind::MalformedData, operation));
    }
    let mut range = CFRange::init(0, 0);
    if !unsafe {
        AXValueGetValue(
            value.as_CFTypeRef().cast_mut().cast(),
            kAXValueTypeCFRange,
            (&mut range as *mut CFRange).cast(),
        )
    } || range.location < 0
        || range.length < 0
    {
        return Err(SelectionError::new(ErrorKind::MalformedData, operation));
    }
    Ok(range)
}

#[repr(C)]
#[derive(Default)]
struct Point {
    x: f64,
    y: f64,
}

#[repr(C)]
#[derive(Default)]
struct Size {
    width: f64,
    height: f64,
}

#[repr(C)]
#[derive(Default)]
struct Rect {
    origin: Point,
    size: Size,
}

fn bounds(
    element: &Element,
    parameter: &CFType,
    context: &Context,
    deadline: Instant,
) -> Vec<SelectionRect> {
    let Ok(Some(value)) = copy_parameterized(
        element,
        kAXBoundsForRangeParameterizedAttribute,
        parameter,
        context,
        deadline,
    ) else {
        return Vec::new();
    };
    if value.type_of() != unsafe { AXValueGetTypeID() }
        || unsafe { AXValueGetType(value.as_CFTypeRef().cast_mut().cast()) } != kAXValueTypeCGRect
    {
        return Vec::new();
    }
    let mut rect = Rect::default();
    if !unsafe {
        AXValueGetValue(
            value.as_CFTypeRef().cast_mut().cast(),
            kAXValueTypeCGRect,
            (&mut rect as *mut Rect).cast(),
        )
    } || ![
        rect.origin.x,
        rect.origin.y,
        rect.size.width,
        rect.size.height,
    ]
    .iter()
    .all(|value| value.is_finite())
        || rect.size.width <= 0.0
        || rect.size.height <= 0.0
    {
        return Vec::new();
    }
    vec![SelectionRect {
        left: rect.origin.x,
        top: rect.origin.y,
        width: rect.size.width,
        height: rect.size.height,
    }]
}

fn checked_text(
    value: CFType,
    operation: &'static str,
    context: &Context,
) -> Result<String, SelectionError> {
    string(value, operation, context.options.max_text_bytes)
}

fn text_for_range(
    element: &Element,
    parameter: &CFType,
    context: &Context,
    deadline: Instant,
) -> Result<Option<String>, SelectionError> {
    copy_parameterized(
        element,
        kAXStringForRangeParameterizedAttribute,
        parameter,
        context,
        deadline,
    )?
    .map(|value| checked_text(value, "AXStringForRange", context))
    .transpose()
}

trait SelectionReader {
    type Range;

    fn multiple(&self) -> Result<Option<Vec<Self::Range>>, SelectionError>;
    fn single(&self) -> Result<Option<Self::Range>, SelectionError>;
    fn length(&self, range: &Self::Range) -> Result<usize, SelectionError>;
    fn text(&self, range: &Self::Range) -> Result<Option<String>, SelectionError>;
    fn bounds(&self, range: &Self::Range) -> Vec<SelectionRect>;
}

struct AxSelectionReader<'a> {
    element: &'a Element,
    context: &'a Context,
    deadline: Instant,
}

impl SelectionReader for AxSelectionReader<'_> {
    type Range = CFType;

    fn multiple(&self) -> Result<Option<Vec<Self::Range>>, SelectionError> {
        let Some(value) = copy_attribute(
            self.element,
            kAXSelectedTextRangesAttribute,
            self.context,
            self.deadline,
        )?
        else {
            return Ok(None);
        };
        let array = value
            .downcast_into::<CFArray<*const c_void>>()
            .ok_or_else(|| SelectionError::new(ErrorKind::MalformedData, "AXSelectedTextRanges"))?;
        let count = usize::try_from(array.len()).map_err(|_| {
            SelectionError::new(ErrorKind::LimitExceeded, "AX selection range count")
        })?;
        if count > MAX_RANGES {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "AX selection range count",
            ));
        }
        Ok(Some(
            array
                .iter()
                .map(|raw| unsafe { CFType::wrap_under_get_rule(*raw) })
                .collect(),
        ))
    }

    fn single(&self) -> Result<Option<Self::Range>, SelectionError> {
        copy_attribute(
            self.element,
            kAXSelectedTextRangeAttribute,
            self.context,
            self.deadline,
        )
    }

    fn length(&self, value: &Self::Range) -> Result<usize, SelectionError> {
        usize::try_from(range(value, "AX selected text range")?.length)
            .map_err(|_| SelectionError::new(ErrorKind::MalformedData, "AX selected text range"))
    }

    fn text(&self, value: &Self::Range) -> Result<Option<String>, SelectionError> {
        text_for_range(self.element, value, self.context, self.deadline)
    }

    fn bounds(&self, value: &Self::Range) -> Vec<SelectionRect> {
        bounds(self.element, value, self.context, self.deadline)
    }
}

fn read_ranges(reader: &impl SelectionReader, limit: usize) -> Result<Probe, SelectionError> {
    let range_values = if let Some(ranges) = reader.multiple()? {
        ranges
    } else if let Some(range) = reader.single()? {
        vec![range]
    } else {
        return Ok(Probe::Unsupported);
    };
    if range_values.len() > MAX_RANGES {
        return Err(SelectionError::new(
            ErrorKind::LimitExceeded,
            "AX selection range count",
        ));
    }

    let mut ranges = Vec::new();
    let mut bytes = 0usize;
    for value in &range_values {
        if reader.length(value)? == 0 {
            continue;
        }
        let Some(text) = reader.text(value)? else {
            return Ok(Probe::Unsupported);
        };
        if text.is_empty() {
            return Err(SelectionError::new(
                ErrorKind::MalformedData,
                "AX selected range text",
            ));
        }
        bytes = bytes
            .checked_add(text.len())
            .and_then(|bytes| bytes.checked_add(usize::from(!ranges.is_empty())))
            .ok_or_else(|| SelectionError::new(ErrorKind::LimitExceeded, "AX selected text"))?;
        if bytes > limit {
            return Err(SelectionError::new(
                ErrorKind::LimitExceeded,
                "AX selected text",
            ));
        }
        ranges.push(SelectedRange {
            text,
            bounds: reader.bounds(value),
        });
    }
    if ranges.is_empty() {
        Ok(Probe::Empty)
    } else {
        Ok(Probe::Selected(ranges))
    }
}

fn selection(
    element: &Element,
    context: &Context,
    deadline: Instant,
) -> Result<Probe, SelectionError> {
    read_ranges(
        &AxSelectionReader {
            element,
            context,
            deadline,
        },
        context.options.max_text_bytes,
    )
}

fn secure(element: &Element, context: &Context, deadline: Instant) -> Result<(), SelectionError> {
    let Some(value) = copy_attribute(element, kAXSubroleAttribute, context, deadline)? else {
        return Ok(());
    };
    ensure_not_secure(&string(value, "AXSubrole", 4096)?)
}

fn ensure_not_secure(subrole: &str) -> Result<(), SelectionError> {
    if subrole == kAXSecureTextFieldSubrole {
        return Err(SelectionError::new(
            ErrorKind::ProtectedContent,
            "AX secure text field",
        ));
    }
    Ok(())
}

fn focused(
    application: &Element,
    context: &Context,
    deadline: Instant,
) -> Result<Element, SelectionError> {
    let value = copy_attribute(application, kAXFocusedUIElementAttribute, context, deadline)?
        .ok_or_else(|| {
            SelectionError::new(ErrorKind::TargetUnavailable, "AX focused UI element")
        })?;
    let element = Element::from_value(value, "AX focused UI element type")?;
    let mut process_id = 0;
    let code = unsafe { AXUIElementGetPid(element.raw(), &mut process_id) };
    if code != AX_ERROR_SUCCESS {
        return Err(error("AXUIElementGetPid", code));
    }
    ensure_focus_process(process_id, context.source.process_id)?;
    Ok(element)
}

fn ensure_focus_process(process_id: i32, expected: u32) -> Result<(), SelectionError> {
    if process_id <= 0 || process_id as u32 != expected {
        return Err(SelectionError::new(
            ErrorKind::TargetChanged,
            "AX focused process identity",
        ));
    }
    Ok(())
}

fn capture_inner(context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
    if !unsafe { AXIsProcessTrusted() } {
        return Err(SelectionError::new(
            ErrorKind::AccessDenied,
            "macOS Accessibility permission",
        ));
    }
    validate(context)?;
    let application = Element::application(context.source.process_id)?;
    let element = focused(&application, context, deadline)?;
    secure(&element, context, deadline)?;
    let result = selection(&element, context, deadline)?;
    context.check()?;
    validate(context)?;
    let current = focused(&application, context, deadline)?;
    if element.0 != current.0 {
        return Err(SelectionError::new(
            ErrorKind::TargetChanged,
            "AX focused UI element changed",
        ));
    }
    secure(&current, context, deadline)?;
    Ok(result)
}

pub(super) fn capture(context: &Context, deadline: Instant) -> Result<Probe, SelectionError> {
    match capture_inner(context, deadline) {
        Err(error) if is_recoverable(&error) => Ok(Probe::Recoverable(error)),
        result => result,
    }
}

pub(super) struct CopyTarget {
    application: Element,
    element: Element,
}

impl CopyTarget {
    pub(super) fn validate(
        &self,
        context: &Context,
        deadline: Instant,
    ) -> Result<(), SelectionError> {
        validate(context)?;
        let current = focused(&self.application, context, deadline)?;
        if self.element.0 != current.0 {
            return Err(SelectionError::new(
                ErrorKind::TargetChanged,
                "AX focused UI element changed",
            ));
        }
        secure(&current, context, deadline)
    }
}

pub(super) fn prepare_copy_target(
    context: &Context,
    deadline: Instant,
) -> Result<CopyTarget, SelectionError> {
    if !unsafe { AXIsProcessTrusted() } {
        return Err(SelectionError::new(
            ErrorKind::AccessDenied,
            "macOS Accessibility permission",
        ));
    }
    validate(context)?;
    let application = Element::application(context.source.process_id)?;
    let element = focused(&application, context, deadline)?;
    secure(&element, context, deadline)?;
    Ok(CopyTarget {
        application,
        element,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone)]
    struct MockRange {
        length: usize,
        text: Option<String>,
        bounds: Vec<SelectionRect>,
    }

    struct MockSelection {
        multiple: Option<Vec<usize>>,
        single: Option<usize>,
        ranges: Vec<MockRange>,
    }

    impl SelectionReader for MockSelection {
        type Range = usize;

        fn multiple(&self) -> Result<Option<Vec<Self::Range>>, SelectionError> {
            Ok(self.multiple.clone())
        }

        fn single(&self) -> Result<Option<Self::Range>, SelectionError> {
            Ok(self.single)
        }

        fn length(&self, range: &Self::Range) -> Result<usize, SelectionError> {
            Ok(self.ranges[*range].length)
        }

        fn text(&self, range: &Self::Range) -> Result<Option<String>, SelectionError> {
            Ok(self.ranges[*range].text.clone())
        }

        fn bounds(&self, range: &Self::Range) -> Vec<SelectionRect> {
            self.ranges[*range].bounds.clone()
        }
    }

    fn mock_range(length: usize, text: Option<&str>) -> MockRange {
        MockRange {
            length,
            text: text.map(str::to_owned),
            bounds: vec![SelectionRect {
                left: 1.0,
                top: 2.0,
                width: 3.0,
                height: 4.0,
            }],
        }
    }

    #[test]
    fn ax_errors_preserve_codes_and_only_known_transient_failures_recover() {
        for (code, kind, recoverable) in [
            (kAXErrorAPIDisabled, ErrorKind::AccessDenied, false),
            (kAXErrorCannotComplete, ErrorKind::TimedOut, true),
            (
                kAXErrorInvalidUIElement,
                ErrorKind::TargetUnavailable,
                false,
            ),
            (kAXErrorIllegalArgument, ErrorKind::MalformedData, false),
            (kAXErrorFailure, ErrorKind::NativeApi, true),
        ] {
            let error = error("test", code);
            assert_eq!(error.kind, kind);
            assert_eq!(error.native_code, Some(code));
            assert_eq!(is_recoverable(&error), recoverable);
        }
        assert!(is_recoverable(&SelectionError::new(
            ErrorKind::TimedOut,
            "Accessibility budget"
        )));
        assert!(!is_recoverable(&SelectionError::new(
            ErrorKind::TimedOut,
            "capture request"
        )));
    }

    #[test]
    fn mocked_focus_and_secure_field_validation_is_terminal() {
        assert!(ensure_focus_process(42, 42).is_ok());
        assert_eq!(
            ensure_focus_process(41, 42).unwrap_err().kind,
            ErrorKind::TargetChanged
        );
        assert_eq!(
            ensure_focus_process(0, 42).unwrap_err().kind,
            ErrorKind::TargetChanged
        );
        assert!(ensure_not_secure("AXSearchField").is_ok());
        assert_eq!(
            ensure_not_secure(kAXSecureTextFieldSubrole)
                .unwrap_err()
                .kind,
            ErrorKind::ProtectedContent
        );
    }

    #[test]
    fn bounded_cfstring_conversion_preserves_unicode_and_embedded_nuls() {
        let payload = "中\0😀";
        assert_eq!(
            string(
                CFString::new(payload).as_CFType(),
                "test string",
                payload.len()
            )
            .unwrap(),
            payload
        );
        assert_eq!(
            string(
                CFString::new(payload).as_CFType(),
                "test string",
                payload.len() - 1,
            )
            .unwrap_err()
            .kind,
            ErrorKind::LimitExceeded
        );
    }

    #[test]
    fn mocked_single_and_multiple_ranges_preserve_unicode_order_and_bounds() {
        let single = MockSelection {
            multiple: None,
            single: Some(0),
            ranges: vec![mock_range(4, Some("你好😀"))],
        };
        let Probe::Selected(ranges) = read_ranges(&single, "你好😀".len()).unwrap() else {
            panic!("single range should be selected")
        };
        assert_eq!(ranges[0].text, "你好😀");
        assert_eq!(ranges[0].bounds[0].left, 1.0);

        let multiple = MockSelection {
            multiple: Some(vec![0, 1, 2]),
            single: None,
            ranges: vec![
                mock_range(2, Some("first")),
                mock_range(0, None),
                mock_range(2, Some("三😀")),
            ],
        };
        let Probe::Selected(ranges) = read_ranges(&multiple, "first\n三😀".len()).unwrap()
        else {
            panic!("multiple ranges should be selected")
        };
        assert_eq!(
            ranges
                .iter()
                .map(|range| range.text.as_str())
                .collect::<Vec<_>>(),
            ["first", "三😀"]
        );
    }

    #[test]
    fn mocked_ranges_distinguish_empty_unsupported_malformed_and_limits() {
        for selection in [
            MockSelection {
                multiple: Some(Vec::new()),
                single: None,
                ranges: Vec::new(),
            },
            MockSelection {
                multiple: Some(vec![0]),
                single: None,
                ranges: vec![mock_range(0, None)],
            },
        ] {
            assert!(matches!(read_ranges(&selection, 16), Ok(Probe::Empty)));
        }

        let unsupported = MockSelection {
            multiple: None,
            single: None,
            ranges: Vec::new(),
        };
        assert!(matches!(
            read_ranges(&unsupported, 16),
            Ok(Probe::Unsupported)
        ));
        let unsupported_text = MockSelection {
            multiple: Some(vec![0]),
            single: None,
            ranges: vec![mock_range(1, None)],
        };
        assert!(matches!(
            read_ranges(&unsupported_text, 16),
            Ok(Probe::Unsupported)
        ));
        let malformed = MockSelection {
            multiple: Some(vec![0]),
            single: None,
            ranges: vec![mock_range(1, Some(""))],
        };
        assert_eq!(
            read_ranges(&malformed, 16).unwrap_err().kind,
            ErrorKind::MalformedData
        );
        let oversized = MockSelection {
            multiple: Some(vec![0, 1]),
            single: None,
            ranges: vec![mock_range(1, Some("ab")), mock_range(1, Some("cd"))],
        };
        assert_eq!(
            read_ranges(&oversized, 4).unwrap_err().kind,
            ErrorKind::LimitExceeded
        );
        let too_many = MockSelection {
            multiple: Some(vec![0; MAX_RANGES + 1]),
            single: None,
            ranges: vec![mock_range(1, Some("x"))],
        };
        assert_eq!(
            read_ranges(&too_many, 1024).unwrap_err().kind,
            ErrorKind::LimitExceeded
        );
    }
}

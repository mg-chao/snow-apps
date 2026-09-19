use super::geometry::{DisplayInfo, Rect, WindowInfo, visible_window};
use super::traversal::{AxProvider, Budget};
use crate::{SelectorResult, StopReason, WindowSnapshot};
use accessibility_sys::*;
use core_foundation::base::{CFType, TCFType};
use core_foundation::boolean::CFBoolean;
use core_foundation::dictionary::CFDictionary;
use core_foundation::string::CFString;
use core_foundation_sys::{
    array::{CFArrayGetCount, CFArrayGetTypeID, CFArrayGetValueAtIndex, CFArrayRef},
    base::{CFGetTypeID, CFTypeRef},
    dictionary::{CFDictionaryGetTypeID, CFDictionaryGetValue, CFDictionaryRef},
    number::{
        CFNumberGetTypeID, CFNumberGetValue, CFNumberRef, kCFNumberDoubleType, kCFNumberSInt64Type,
    },
};
use std::ptr;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct CGPoint {
    x: f64,
    y: f64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct CGSize {
    width: f64,
    height: f64,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct CGRect {
    origin: CGPoint,
    size: CGSize,
}
impl From<CGRect> for Rect {
    fn from(r: CGRect) -> Self {
        Self {
            x: r.origin.x,
            y: r.origin.y,
            width: r.size.width,
            height: r.size.height,
        }
    }
}
#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {
    fn CGWindowListCopyWindowInfo(options: u32, relative: u32) -> CFArrayRef;
    fn CGRectMakeWithDictionaryRepresentation(dict: CFDictionaryRef, rect: *mut CGRect) -> bool;
    fn CGGetActiveDisplayList(max: u32, displays: *mut u32, count: *mut u32) -> i32;
    fn CGDisplayBounds(id: u32) -> CGRect;
    fn CGDisplayCopyDisplayMode(id: u32) -> CFTypeRef;
    fn CGDisplayModeGetPixelWidth(mode: CFTypeRef) -> usize;
    fn CGDisplayModeGetWidth(mode: CFTypeRef) -> usize;
    fn CGDisplayModeGetHeight(mode: CFTypeRef) -> usize;
    fn CGDisplayModeGetPixelHeight(mode: CFTypeRef) -> usize;
}
/// Only explicit user actions pass `prompt=true`. Snapshot and hover paths always use false.
pub fn accessibility_permission(prompt: bool) -> bool {
    if !prompt {
        return unsafe { AXIsProcessTrusted() };
    }
    let key = unsafe { CFString::wrap_under_get_rule(kAXTrustedCheckOptionPrompt) };
    let options = CFDictionary::from_CFType_pairs(&[(key, CFBoolean::true_value())]);
    unsafe { AXIsProcessTrustedWithOptions(options.as_concrete_TypeRef()) }
}
fn owned(value: CFTypeRef) -> Option<CFType> {
    (!value.is_null()).then(|| unsafe { CFType::wrap_under_create_rule(value) })
}
fn dictionary_value(dict: CFDictionaryRef, name: &str) -> CFTypeRef {
    let key = CFString::new(name);
    unsafe { CFDictionaryGetValue(dict, key.as_concrete_TypeRef().cast()) }
}
fn integer(dict: CFDictionaryRef, name: &str) -> Option<i64> {
    let value = dictionary_value(dict, name);
    if value.is_null() || unsafe { CFGetTypeID(value) } != unsafe { CFNumberGetTypeID() } {
        return None;
    }
    let mut out = 0i64;
    unsafe {
        CFNumberGetValue(
            value as CFNumberRef,
            kCFNumberSInt64Type,
            (&mut out as *mut i64).cast(),
        )
    }
    .then_some(out)
}
fn number(dict: CFDictionaryRef, name: &str) -> Option<f64> {
    let value = dictionary_value(dict, name);
    if value.is_null() || unsafe { CFGetTypeID(value) } != unsafe { CFNumberGetTypeID() } {
        return None;
    }
    let mut out = 0f64;
    unsafe {
        CFNumberGetValue(
            value as CFNumberRef,
            kCFNumberDoubleType,
            (&mut out as *mut f64).cast(),
        )
    }
    .then_some(out)
}
pub(super) fn snapshot(excluded: &[usize]) -> SelectorResult<WindowSnapshot> {
    let mut count = 0;
    if unsafe { CGGetActiveDisplayList(0, ptr::null_mut(), &mut count) } != 0
        || count == 0
        || count > 128
    {
        return Err("could not enumerate active displays".into());
    }
    let mut ids = vec![0; count as usize];
    if unsafe { CGGetActiveDisplayList(count, ids.as_mut_ptr(), &mut count) } != 0 {
        return Err("could not read active displays".into());
    }
    let mut displays = Vec::new();
    for id in ids.into_iter().take(count as usize) {
        let mode = owned(unsafe { CGDisplayCopyDisplayMode(id) }).ok_or("missing display mode")?;
        let display = DisplayInfo::from_mode(
            id,
            unsafe { CGDisplayBounds(id) }.into(),
            unsafe { CGDisplayModeGetWidth(mode.as_CFTypeRef()) },
            unsafe { CGDisplayModeGetHeight(mode.as_CFTypeRef()) },
            unsafe { CGDisplayModeGetPixelWidth(mode.as_CFTypeRef()) },
            unsafe { CGDisplayModeGetPixelHeight(mode.as_CFTypeRef()) },
        );
        if let Some(display) = display {
            displays.push(display);
        }
    }
    if displays.is_empty() {
        return Err("no valid display geometry".into());
    }
    // On-screen only | exclude desktop elements. Quartz returns front-to-back order.
    let list = owned(unsafe { CGWindowListCopyWindowInfo(1 | 16, 0) }.cast())
        .ok_or("could not enumerate windows")?;
    if list.type_of() != unsafe { CFArrayGetTypeID() } {
        return Err("invalid window list".into());
    }
    let array = list.as_CFTypeRef() as CFArrayRef;
    let mut windows = Vec::new();
    for i in 0..unsafe { CFArrayGetCount(array) } {
        let value = unsafe { CFArrayGetValueAtIndex(array, i) };
        if value.is_null() || unsafe { CFGetTypeID(value) } != unsafe { CFDictionaryGetTypeID() } {
            continue;
        }
        let dict = value as CFDictionaryRef;
        let Some(id) = integer(dict, "kCGWindowNumber").and_then(|n| usize::try_from(n).ok())
        else {
            continue;
        };
        let Some(pid) = integer(dict, "kCGWindowOwnerPID").and_then(|n| i32::try_from(n).ok())
        else {
            continue;
        };
        let Some(layer) = integer(dict, "kCGWindowLayer").and_then(|n| i32::try_from(n).ok())
        else {
            continue;
        };
        let Some(alpha) = number(dict, "kCGWindowAlpha") else {
            continue;
        };
        let bounds = dictionary_value(dict, "kCGWindowBounds");
        if bounds.is_null() || unsafe { CFGetTypeID(bounds) } != unsafe { CFDictionaryGetTypeID() }
        {
            continue;
        }
        let mut rect = CGRect::default();
        if !unsafe { CGRectMakeWithDictionaryRepresentation(bounds as CFDictionaryRef, &mut rect) }
        {
            continue;
        }
        if let Some(window) =
            visible_window(id, pid, rect.into(), alpha, layer, excluded).filter(|w| {
                displays
                    .iter()
                    .any(|d| w.bounds.intersect(d.bounds).is_some())
            })
        {
            windows.push(window);
        }
    }
    Ok(WindowSnapshot { windows, displays })
}

#[derive(Clone, PartialEq)]
pub(super) struct Element(CFType);
impl Element {
    fn from_value(value: CFType) -> Result<Self, StopReason> {
        if value.type_of() != unsafe { AXUIElementGetTypeID() } {
            return Err(StopReason::ProviderFailure);
        }
        Ok(Self(value))
    }
    fn raw(&self) -> AXUIElementRef {
        self.0.as_CFTypeRef().cast_mut().cast()
    }
    fn timeout(&self, budget: &Budget<'_>) -> Result<(), StopReason> {
        let remaining = budget.check()?;
        status(unsafe {
            AXUIElementSetMessagingTimeout(self.raw(), remaining.as_secs_f32().max(0.001))
        })
    }
    fn attribute(&self, name: &str, budget: &Budget<'_>) -> Result<Option<CFType>, StopReason> {
        self.timeout(budget)?;
        let key = CFString::new(name);
        let mut raw = ptr::null();
        let code = unsafe {
            AXUIElementCopyAttributeValue(self.raw(), key.as_concrete_TypeRef(), &mut raw)
        };
        let value = owned(raw);
        budget.check()?;
        if code == kAXErrorAttributeUnsupported
            || code == kAXErrorNoValue
            || code == kAXErrorNotImplemented
        {
            return Ok(None);
        }
        status(code)?;
        value.map(Some).ok_or(StopReason::ProviderFailure)
    }
}
fn status(code: AXError) -> Result<(), StopReason> {
    if code == kAXErrorSuccess {
        Ok(())
    } else if code == kAXErrorAPIDisabled || !accessibility_permission(false) {
        Err(StopReason::PermissionRequired)
    } else if code == kAXErrorCannotComplete {
        Err(StopReason::ProviderTimeout)
    } else {
        Err(StopReason::ProviderFailure)
    }
}
fn decode<T: Default>(value: &CFType, kind: AXValueType) -> Result<T, StopReason> {
    if value.type_of() != unsafe { AXValueGetTypeID() } {
        return Err(StopReason::ProviderFailure);
    }
    let raw = value.as_CFTypeRef().cast_mut().cast();
    if unsafe { AXValueGetType(raw) } != kind {
        return Err(StopReason::ProviderFailure);
    }
    let mut result = T::default();
    if !unsafe { AXValueGetValue(raw, kind, (&mut result as *mut T).cast()) } {
        return Err(StopReason::ProviderFailure);
    }
    Ok(result)
}
pub(super) struct Provider;
impl AxProvider for Provider {
    type Element = Element;
    fn trusted(&self) -> bool {
        accessibility_permission(false)
    }
    fn hit(
        &mut self,
        window: &WindowInfo,
        (x, y): (f64, f64),
        budget: &Budget<'_>,
    ) -> Result<Element, StopReason> {
        // Target the cached owner directly so excluded capture overlays cannot steal
        // the hit. Setting a system-wide AX timeout would change process-global state
        // shared by the foreground and refinement workers.
        let app = Element::from_value(
            owned(unsafe { AXUIElementCreateApplication(window.pid) }.cast())
                .ok_or(StopReason::ProviderFailure)?,
        )?;
        app.timeout(budget)?;
        let mut raw = ptr::null_mut();
        let code =
            unsafe { AXUIElementCopyElementAtPosition(app.raw(), x as f32, y as f32, &mut raw) };
        let value = owned(raw.cast());
        status(code)?;
        budget.check()?;
        Element::from_value(value.ok_or(StopReason::ProviderFailure)?)
    }

    fn window(&mut self, e: &Element, b: &Budget<'_>) -> Result<Element, StopReason> {
        if self.is_window(e, b)? {
            return Ok(e.clone());
        }
        Element::from_value(
            e.attribute(kAXWindowAttribute, b)?
                .ok_or(StopReason::ProviderFailure)?,
        )
    }
    fn pid(&mut self, e: &Element, b: &Budget<'_>) -> Result<i32, StopReason> {
        e.timeout(b)?;
        let mut pid = 0;
        status(unsafe { AXUIElementGetPid(e.raw(), &mut pid) })?;
        b.check()?;
        Ok(pid)
    }
    fn bounds(&mut self, e: &Element, b: &Budget<'_>) -> Result<Option<Rect>, StopReason> {
        let Some(p) = e.attribute(kAXPositionAttribute, b)? else {
            return Ok(None);
        };
        let Some(s) = e.attribute(kAXSizeAttribute, b)? else {
            return Ok(None);
        };
        let p: CGPoint = decode(&p, kAXValueTypeCGPoint)?;
        let s: CGSize = decode(&s, kAXValueTypeCGSize)?;
        let rect = Rect {
            x: p.x,
            y: p.y,
            width: s.width,
            height: s.height,
        };
        Ok(rect.valid().then_some(rect))
    }
    fn is_window(&mut self, e: &Element, b: &Budget<'_>) -> Result<bool, StopReason> {
        let value = e
            .attribute(kAXRoleAttribute, b)?
            .ok_or(StopReason::ProviderFailure)?;
        let role = value
            .downcast_into::<CFString>()
            .ok_or(StopReason::ProviderFailure)?;
        Ok(role == CFString::new(kAXWindowRole))
    }
    fn parent(&mut self, e: &Element, b: &Budget<'_>) -> Result<Option<Element>, StopReason> {
        e.attribute(kAXParentAttribute, b)?
            .map(Element::from_value)
            .transpose()
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ax_values_require_correct_type_and_payload() {
        let p = CGPoint { x: -12.5, y: 3.25 };
        let value = owned(
            unsafe { AXValueCreate(kAXValueTypeCGPoint, (&p as *const CGPoint).cast()) }.cast(),
        )
        .unwrap();
        let decoded: CGPoint = decode(&value, kAXValueTypeCGPoint).unwrap();
        assert_eq!((decoded.x, decoded.y), (-12.5, 3.25));
        assert!(decode::<CGSize>(&value, kAXValueTypeCGSize).is_err());
        assert!(decode::<CGPoint>(&CFString::new("bad").as_CFType(), kAXValueTypeCGPoint).is_err());
    }
}

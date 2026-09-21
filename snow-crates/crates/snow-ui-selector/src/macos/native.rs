use super::activation::Backend;
use super::activation_flags::{self, ActivationState, Flag, Flags};
use super::geometry::{DisplayInfo, Rect, WindowInfo, visible_window};
use super::traversal::{AxProvider, Budget, CHILD_BATCH, Children};
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
use std::ffi::{CStr, c_void};
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
    fn CGWindowLevelForKey(key: i32) -> i32;
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
#[link(name = "proc")]
unsafe extern "C" {
    fn proc_pidpath(pid: i32, buffer: *mut c_void, buffersize: u32) -> i32;
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
pub(super) fn is_dock_surface(layer: i32, dock_layer: i32, executable: Option<&str>) -> bool {
    // Level alone is not ownership, and Quartz owner names are localized. Identify the
    // system Dock executable, preserving application panels that happen to use its level.
    layer == dock_layer
        && executable == Some("/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock")
}

fn process_executable(pid: i32) -> Option<String> {
    const PROC_PIDPATHINFO_MAXSIZE: usize = 4096;
    let mut buffer = [0u8; PROC_PIDPATHINFO_MAXSIZE];
    let length = unsafe { proc_pidpath(pid, buffer.as_mut_ptr().cast(), buffer.len() as u32) };
    if length <= 0 {
        return None;
    }
    CStr::from_bytes_until_nul(&buffer)
        .ok()?
        .to_str()
        .ok()
        .map(str::to_owned)
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
    const DOCK_WINDOW_LEVEL_KEY: i32 = 7; // kCGDockWindowLevelKey
    let dock_layer = unsafe { CGWindowLevelForKey(DOCK_WINDOW_LEVEL_KEY) };
    let mut dock_owners = std::collections::HashMap::new();
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
        if layer == dock_layer
            && *dock_owners.entry(pid).or_insert_with(|| {
                is_dock_surface(layer, dock_layer, process_executable(pid).as_deref())
            })
        {
            continue;
        }
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
    Ok(WindowSnapshot {
        windows,
        displays,
        ..WindowSnapshot::default()
    })
}

#[derive(Clone, PartialEq)]
pub(super) struct Element(CFType);
impl Element {
    fn application(pid: i32) -> Result<Self, StopReason> {
        Self::from_value(
            owned(unsafe { AXUIElementCreateApplication(pid) }.cast())
                .ok_or(StopReason::ProviderFailure)?,
        )
    }
    fn elements(
        &self,
        name: &str,
        offset: usize,
        budget: &Budget<'_>,
    ) -> Result<Option<Children<Element>>, StopReason> {
        self.timeout(budget)?;
        let key = CFString::new(name);
        let mut count = 0;
        let code = unsafe {
            AXUIElementGetAttributeValueCount(self.raw(), key.as_concrete_TypeRef(), &mut count)
        };
        budget.check()?;
        if code == kAXErrorAttributeUnsupported || code == kAXErrorNotImplemented {
            return Ok(None);
        }
        if code == kAXErrorNoValue
            || code == kAXErrorInvalidUIElement
            || offset >= count.max(0) as usize
        {
            // Do not hide errors just because the count out-parameter stayed zero.
            if code != kAXErrorNoValue && code != kAXErrorInvalidUIElement {
                status(code)?;
            }
            return Ok(Some(Children {
                elements: vec![],
                more: false,
            }));
        }
        status(code)?;
        let length = CHILD_BATCH.min(count as usize - offset);
        self.timeout(budget)?;
        let mut raw = ptr::null();
        let code = unsafe {
            AXUIElementCopyAttributeValues(
                self.raw(),
                key.as_concrete_TypeRef(),
                offset as isize,
                length as isize,
                &mut raw,
            )
        };
        let value = owned(raw.cast());
        budget.check()?;
        if code == kAXErrorInvalidUIElement
            || code == kAXErrorIllegalArgument
            || code == kAXErrorNoValue
        {
            return Ok(Some(Children {
                elements: vec![],
                more: false,
            }));
        }
        status(code)?;
        let value = value.ok_or(StopReason::ProviderFailure)?;
        if value.type_of() != unsafe { CFArrayGetTypeID() } {
            return Err(StopReason::ProviderFailure);
        }
        let array = value.as_CFTypeRef() as CFArrayRef;
        let count_returned = unsafe { CFArrayGetCount(array) } as usize;
        if count_returned > length {
            return Err(StopReason::ProviderFailure);
        }
        let mut elements = Vec::with_capacity(count_returned);
        for index in 0..count_returned {
            let raw = unsafe { CFArrayGetValueAtIndex(array, index as isize) };
            if raw.is_null() {
                return Err(StopReason::ProviderFailure);
            }
            elements.push(Self::from_value(unsafe {
                CFType::wrap_under_get_rule(raw)
            })?);
        }
        Ok(Some(Children {
            elements,
            more: offset + count_returned < count as usize,
        }))
    }
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
            || code == kAXErrorInvalidUIElement
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
// These roles expose actual controls/content at the queried branch. Generic
// containers alone do not establish that an asynchronously created tree is ready.
fn is_concrete_control(role: &str) -> bool {
    matches!(
        role,
        "AXButton"
            | "AXCheckBox"
            | "AXRadioButton"
            | "AXPopUpButton"
            | "AXMenuButton"
            | "AXTextField"
            | "AXTextArea"
            | "AXStaticText"
            | "AXLink"
            | "AXImage"
            | "AXSlider"
            | "AXIncrementor"
            | "AXComboBox"
            | "AXColorWell"
            | "AXProgressIndicator"
            | "AXMenuItem"
            | "AXCell"
    )
}
#[derive(Default)]
pub(super) struct Provider {
    pub web_content: bool,
    pub concrete_control: bool,
}
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
    ) -> Result<Option<Element>, StopReason> {
        // Target the cached owner directly so excluded capture overlays cannot steal
        // the hit. Setting a system-wide AX timeout would change process-global state
        // shared by the foreground and refinement workers.
        let app = Element::application(window.pid)?;
        app.timeout(budget)?;
        let mut raw = ptr::null_mut();
        let code =
            unsafe { AXUIElementCopyElementAtPosition(app.raw(), x as f32, y as f32, &mut raw) };
        let value = owned(raw.cast());
        budget.check()?;
        if code == kAXErrorNotImplemented
            || code == kAXErrorNoValue
            || code == kAXErrorAttributeUnsupported
        {
            return Ok(None);
        }
        status(code)?;
        Element::from_value(value.ok_or(StopReason::ProviderFailure)?).map(Some)
    }

    fn window_at(
        &mut self,
        window: &WindowInfo,
        budget: &Budget<'_>,
    ) -> Result<Element, StopReason> {
        let app = Element::application(window.pid)?;
        // Some providers expose a tree but no native hit testing. Locate the
        // exact snapshot window, not the application's currently focused one.
        let mut offset = 0;
        while offset < super::traversal::MAX_NODES {
            let Some(page) = app.elements(kAXWindowsAttribute, offset, budget)? else {
                break;
            };
            offset += page.elements.len();
            for candidate in page.elements {
                if self.pid(&candidate, budget)? == window.pid
                    && self.is_window(&candidate, budget)?
                    && self
                        .bounds(&candidate, budget)?
                        .is_some_and(|r| r.matches(window.bounds))
                {
                    return Ok(candidate);
                }
            }
            if !page.more {
                break;
            }
        }
        Err(StopReason::ProviderFailure)
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
        let Some(value) = e.attribute(kAXRoleAttribute, b)? else {
            return Ok(false);
        };
        let role = value
            .downcast_into::<CFString>()
            .ok_or(StopReason::ProviderFailure)?;
        self.web_content |= role == CFString::new("AXWebArea");
        self.concrete_control |= is_concrete_control(&role.to_string());
        Ok(role == CFString::new(kAXWindowRole))
    }
    fn hidden(&mut self, e: &Element, b: &Budget<'_>) -> Result<bool, StopReason> {
        Ok(e.attribute("AXHidden", b)?
            .and_then(|v| boolean(&v))
            .unwrap_or(false))
    }
    fn children(
        &mut self,
        e: &Element,
        offset: usize,
        b: &Budget<'_>,
    ) -> Result<Children<Element>, StopReason> {
        if let Some(page) = e.elements(kAXVisibleChildrenAttribute, offset, b)? {
            return Ok(page);
        }
        Ok(e.elements(kAXChildrenAttribute, offset, b)?
            .unwrap_or(Children {
                elements: vec![],
                more: false,
            }))
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
    fn process_identity_uses_executable_path() {
        let executable = process_executable(std::process::id() as i32).unwrap();
        assert_eq!(
            std::path::Path::new(&executable).file_name(),
            std::env::current_exe().unwrap().file_name()
        );
        assert!(process_executable(-1).is_none());
    }

    #[test]
    fn native_process_identity_has_a_start_time_and_rejects_missing_processes() {
        let backend = Activation::default();
        let identity = backend.identity(std::process::id() as i32).unwrap();
        assert!(identity.started.0 > 0);
        assert_eq!(backend.identity(identity.pid), Some(identity));
        assert!(backend.identity(-1).is_none());
    }
    #[test]
    fn overlapping_sessions_share_owned_activation_until_last_release() {
        use super::super::activation::{Identity, Outcome};
        let id = Identity {
            pid: -456,
            started: (123, 0),
        };
        let first = Activation::default();
        let second = Activation::default();
        // Seed an owned activation without contacting any application.
        let record = activation_record(id);
        *record.lock().unwrap() = ActivationOwners {
            users: 1,
            state: ActivationState {
                outcome: Outcome::Owned,
                flag: Some(Flag::Enhanced),
            },
        };
        first.leases.lock().unwrap().insert(id, record.clone());
        let control = crate::QueryControl::foreground();
        assert_eq!(
            second.enable(id, &Budget::new(&control)).unwrap(),
            Outcome::Owned
        );
        assert_eq!(record.lock().unwrap().users, 2);
        assert_eq!(record.lock().unwrap().state.flag, Some(Flag::Enhanced));
        first.restore(id);
        assert_eq!(record.lock().unwrap().users, 1);
        second.restore(id);
        assert_eq!(record.lock().unwrap().users, 0);
    }
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

fn boolean(value: &CFType) -> Option<bool> {
    if let Some(value) = value.downcast::<CFBoolean>() {
        return Some(value.into());
    }
    if value.type_of() == unsafe { CFNumberGetTypeID() } {
        let mut out = 0i64;
        if unsafe {
            CFNumberGetValue(
                value.as_CFTypeRef() as CFNumberRef,
                kCFNumberSInt64Type,
                (&mut out as *mut i64).cast(),
            )
        } {
            return Some(out != 0);
        }
    }
    None
}

// Per-process leases prevent a closing capture from disabling accessibility
// that a newer capture is already using. This registry contains no AX handles.
type ActivationRecord = std::sync::Arc<std::sync::Mutex<ActivationOwners>>;
#[derive(Debug)]
struct ActivationOwners {
    users: usize,
    state: ActivationState,
}
#[derive(Default, Debug)]
pub(crate) struct Activation {
    leases:
        std::sync::Mutex<std::collections::HashMap<super::activation::Identity, ActivationRecord>>,
}
fn activation_record(id: super::activation::Identity) -> ActivationRecord {
    type Registry = std::collections::HashMap<
        super::activation::Identity,
        std::sync::Weak<std::sync::Mutex<ActivationOwners>>,
    >;
    static REGISTRY: std::sync::OnceLock<std::sync::Mutex<Registry>> = std::sync::OnceLock::new();
    let mut registry = REGISTRY.get_or_init(Default::default).lock().unwrap();
    registry.retain(|_, value| value.strong_count() > 0);
    if let Some(record) = registry.get(&id).and_then(std::sync::Weak::upgrade) {
        return record;
    }
    let record = std::sync::Arc::new(std::sync::Mutex::new(ActivationOwners {
        users: 0,
        state: ActivationState::default(),
    }));
    registry.insert(id, std::sync::Arc::downgrade(&record));
    record
}
impl super::activation::Backend for Activation {
    fn identity(&self, pid: i32) -> Option<super::activation::Identity> {
        // Darwin's public proc_bsdinfo (PROC_PIDTBSDINFO). Start time distinguishes PID reuse.
        #[repr(C)]
        struct BsdInfo {
            ids: [u32; 12],
            comm: [u8; 16],
            name: [u8; 32],
            fields: [u32; 6],
            seconds: u64,
            micros: u64,
        }
        unsafe extern "C" {
            fn proc_pidinfo(pid: i32, flavor: i32, arg: u64, buffer: *mut c_void, size: i32)
            -> i32;
        }
        let mut info: BsdInfo = unsafe { std::mem::zeroed() };
        let size = std::mem::size_of::<BsdInfo>() as i32;
        if unsafe { proc_pidinfo(pid, 3, 0, (&mut info as *mut BsdInfo).cast(), size) } != size {
            return None;
        }
        Some(super::activation::Identity {
            pid,
            started: (info.seconds, info.micros),
        })
    }
    fn enable(
        &self,
        id: super::activation::Identity,
        budget: &Budget<'_>,
    ) -> Result<super::activation::Outcome, StopReason> {
        let record = activation_record(id);
        let mut owners = loop {
            budget.check()?;
            match record.try_lock() {
                Ok(owners) => break owners,
                Err(std::sync::TryLockError::WouldBlock) => {
                    std::thread::sleep(std::time::Duration::from_millis(1))
                }
                Err(std::sync::TryLockError::Poisoned(_)) => {
                    return Err(StopReason::ProviderFailure);
                }
            }
        };
        if owners.users == 0 {
            owners.state = self.enable_native(id, budget)?;
        }
        owners.users += 1;
        let outcome = owners.state.outcome;
        self.leases.lock().unwrap().insert(id, record.clone());
        Ok(outcome)
    }
    fn restore(&self, id: super::activation::Identity) {
        let Some(record) = self.leases.lock().unwrap().remove(&id) else {
            return;
        };
        let mut owners = record.lock().unwrap();
        owners.users -= 1;
        if owners.users == 0
            && owners.state.outcome == super::activation::Outcome::Owned
            && let Some(flag) = owners.state.flag
        {
            self.restore_native(id, flag);
        }
    }
}
struct NativeFlags<'a, 'b> {
    backend: &'a Activation,
    id: super::activation::Identity,
    app: Element,
    budget: &'a Budget<'b>,
}
impl Flags for NativeFlags<'_, '_> {
    fn read(&self, flag: Flag) -> Result<Option<bool>, StopReason> {
        Ok(self
            .app
            .attribute(flag.name(), self.budget)?
            .and_then(|v| boolean(&v)))
    }
    fn writable(&self, flag: Flag) -> Result<bool, StopReason> {
        self.app.timeout(self.budget)?;
        let key = CFString::new(flag.name());
        let mut writable = 0;
        let code = unsafe {
            AXUIElementIsAttributeSettable(self.app.raw(), key.as_concrete_TypeRef(), &mut writable)
        };
        self.budget.check()?;
        if code == kAXErrorAttributeUnsupported || code == kAXErrorNotImplemented {
            return Ok(false);
        }
        status(code)?;
        Ok(writable != 0)
    }
    fn write(&self, flag: Flag, enabled: bool) -> Result<(), StopReason> {
        if self.backend.identity(self.id.pid) != Some(self.id) {
            return Err(StopReason::ProviderFailure);
        }
        self.app.timeout(self.budget)?;
        let key = CFString::new(flag.name());
        let value = if enabled {
            CFBoolean::true_value()
        } else {
            CFBoolean::false_value()
        };
        let code = unsafe {
            AXUIElementSetAttributeValue(
                self.app.raw(),
                key.as_concrete_TypeRef(),
                value.as_CFTypeRef(),
            )
        };
        // Success must retain ownership even if the write exhausted the budget.
        activation_flags::verify_write(flag, enabled, code, || self.read(flag), status)
    }
    fn rollback(&self, flag: Flag) {
        // Cleanup has its own bounded budget even after cancellation or timeout.
        self.backend.restore_native(self.id, flag);
    }
}
impl Activation {
    fn enable_native(
        &self,
        id: super::activation::Identity,
        budget: &Budget<'_>,
    ) -> Result<ActivationState, StopReason> {
        activation_flags::enable(&NativeFlags {
            backend: self,
            id,
            app: Element::application(id.pid)?,
            budget,
        })
    }
    fn restore_native(&self, id: super::activation::Identity, flag: Flag) {
        if self.identity(id.pid) != Some(id) {
            return;
        }
        let control = crate::QueryControl::foreground();
        let budget = Budget::new(&control);
        let result = (|| {
            let flags = NativeFlags {
                backend: self,
                id,
                app: Element::application(id.pid)?,
                budget: &budget,
            };
            match flags.read(flag)? {
                Some(false) => Ok(()),
                Some(true) => flags.write(flag, false),
                None => Err(StopReason::ProviderFailure),
            }
        })();
        if let Err(reason) = result {
            eprintln!(
                "selector activation restore pid={} flag={} reason={reason:?}",
                id.pid,
                flag.name()
            );
        }
    }
}

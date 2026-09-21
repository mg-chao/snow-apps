//! TIS input-source APIs belong to the main queue. Workers use an owned,
//! aligned snapshot so preview shutdown can join them without a queue cycle.
use objc2::MainThreadMarker;
use objc2_core_foundation::{
    CFData, CFDictionary, CFNotificationCenter, CFNotificationName,
    CFNotificationSuspensionBehavior, CFRetained, CFString, CFType,
};
use std::{
    ffi::c_void,
    ptr::NonNull,
    sync::{Arc, Once, RwLock},
};

static PREPARED: Once = Once::new();
static LAYOUT: RwLock<Option<Arc<[u32]>>> = RwLock::new(None);

#[link(name = "Carbon", kind = "framework")]
unsafe extern "C" {
    static kTISPropertyUnicodeKeyLayoutData: *const CFString;
    static kTISNotifySelectedKeyboardInputSourceChanged: *const CFString;
    fn TISCopyCurrentKeyboardLayoutInputSource() -> *mut CFType;
    fn TISGetInputSourceProperty(source: *const CFType, property: *const CFString)
    -> *const c_void;
}

pub(crate) fn prepare() {
    if PREPARED.is_completed() {
        return;
    }
    let initialize = || {
        // Acquire Once only on the main thread. A worker waiting for dispatch
        // must never hold it while a main-thread caller tries to initialize.
        PREPARED.call_once(|| {
            let main = MainThreadMarker::new().expect("TIS requires the main thread");
            if let Some(center) = CFNotificationCenter::distributed_center() {
                // Process-lifetime observer with static context; no session or
                // worker is captured, and layout changes apply to all consumers.
                unsafe {
                    center.add_observer(
                        (&raw const LAYOUT).cast(),
                        Some(layout_changed),
                        kTISNotifySelectedKeyboardInputSourceChanged.as_ref(),
                        std::ptr::null(),
                        CFNotificationSuspensionBehavior::DeliverImmediately,
                    );
                }
            }
            refresh(main);
        });
    };
    if MainThreadMarker::new().is_some() {
        initialize();
    } else {
        dispatch2::DispatchQueue::main().exec_sync(initialize);
    }
}

unsafe extern "C-unwind" fn layout_changed(
    _: *mut CFNotificationCenter,
    _: *mut c_void,
    _: *const CFNotificationName,
    _: *const c_void,
    _: *const CFDictionary,
) {
    // Distributed notifications are delivered on the main run loop.
    refresh(MainThreadMarker::new().expect("TIS notifications require the main thread"));
}

fn refresh(_main: MainThreadMarker) {
    let copy = || unsafe {
        let source = CFRetained::from_raw(NonNull::new(TISCopyCurrentKeyboardLayoutInputSource())?);
        let data = TISGetInputSourceProperty(
            CFRetained::as_ptr(&source).as_ptr(),
            kTISPropertyUnicodeKeyLayoutData,
        )
        .cast::<CFData>()
        .as_ref()?;
        let length = usize::try_from(data.length())
            .ok()
            .filter(|length| *length > 0)?;
        // UCKeyboardLayout contains UInt32 fields; retain neither the TIS
        // source nor its borrowed property on a recording worker.
        let mut owned = vec![0u32; length.div_ceil(4)];
        std::ptr::copy_nonoverlapping(data.byte_ptr(), owned.as_mut_ptr().cast(), length);
        Some(Arc::from(owned))
    };
    let layout = copy();
    *LAYOUT.write().unwrap() = layout;
}

pub(crate) fn snapshot() -> Option<Arc<[u32]>> {
    LAYOUT.read().unwrap().clone()
}

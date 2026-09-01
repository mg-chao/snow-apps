use std::panic::{AssertUnwindSafe, catch_unwind};

use snow_draw_engine::{Runtime, ViewportId};

use crate::abi::patch::SnowPatchPayload;
use crate::abi::types::SnowError;

pub type SnowRuntime = *mut SnowRuntimeImpl;
pub type SnowViewport = *mut SnowViewportImpl;
pub type SnowPatchHandle = *mut SnowPatchHandleImpl;
pub type SnowChangedViewportList = *mut SnowChangedViewportListImpl;

pub struct SnowRuntimeImpl {
    pub(crate) runtime: Runtime,
}

pub struct SnowViewportImpl {
    pub(crate) id: ViewportId,
}

pub struct SnowPatchHandleImpl {
    pub(crate) payload: SnowPatchPayload,
}

pub struct SnowChangedViewportListImpl {
    pub(crate) viewport_ids: Box<[u64]>,
}

pub(crate) fn ffi_status(result: Result<(), SnowError>) -> SnowError {
    match result {
        Ok(()) => SnowError::Ok,
        Err(error) => error,
    }
}

pub(crate) fn ffi_error(f: impl FnOnce() -> SnowError) -> SnowError {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(error) => error,
        Err(_) => SnowError::Internal,
    }
}

pub(crate) fn ffi_value<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(value) => value,
        Err(_) => fallback,
    }
}

pub(crate) fn ffi_void(f: impl FnOnce()) {
    let _ = catch_unwind(AssertUnwindSafe(f));
}

fn slice_ptr_or_null<T>(slice: &[T]) -> *const T {
    if slice.is_empty() {
        std::ptr::null()
    } else {
        slice.as_ptr()
    }
}

pub(crate) fn write_slice_out<T>(out_items: *mut *const T, out_count: *mut u32, items: &[T]) {
    write_out(out_items, slice_ptr_or_null(items));
    write_out(out_count, items.len() as u32);
}

pub(crate) fn write_changed_viewports(
    out_changed_viewports: *mut SnowChangedViewportList,
    changed_viewports: Vec<ViewportId>,
) {
    let viewport_ids = changed_viewports
        .into_iter()
        .map(|viewport_id| viewport_id.0)
        .collect::<Vec<_>>()
        .into_boxed_slice();
    write_out(
        out_changed_viewports,
        Box::into_raw(Box::new(SnowChangedViewportListImpl { viewport_ids })),
    );
}

pub(crate) fn write_out<T>(ptr: *mut T, value: T) {
    unsafe {
        *ptr = value;
    }
}

pub(crate) fn with_runtime_impl_mut<T>(
    runtime: SnowRuntime,
    f: impl FnOnce(&mut SnowRuntimeImpl) -> Result<T, SnowError>,
) -> Result<T, SnowError> {
    if runtime.is_null() {
        return Err(SnowError::InvalidArgument);
    }
    let runtime = unsafe { &mut *runtime };
    f(runtime)
}

pub(crate) fn with_runtime_ref<T>(
    runtime: SnowRuntime,
    f: impl FnOnce(&Runtime) -> Result<T, SnowError>,
) -> Result<T, SnowError> {
    if runtime.is_null() {
        return Err(SnowError::InvalidArgument);
    }
    let runtime = unsafe { &*runtime };
    f(&runtime.runtime)
}

pub(crate) fn viewport_id(viewport: SnowViewport) -> Result<ViewportId, SnowError> {
    if viewport.is_null() {
        return Err(SnowError::InvalidArgument);
    }
    Ok(unsafe { (*viewport).id })
}

pub(crate) fn with_runtime_viewport_ref<T>(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    f: impl FnOnce(&Runtime, ViewportId) -> Result<T, SnowError>,
) -> Result<T, SnowError> {
    let id = viewport_id(viewport)?;
    with_runtime_ref(runtime, |runtime| f(runtime, id))
}

pub(crate) fn with_runtime_viewport_mut<T>(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    f: impl FnOnce(&mut Runtime, ViewportId) -> Result<T, SnowError>,
) -> Result<T, SnowError> {
    let id = viewport_id(viewport)?;
    with_runtime_impl_mut(runtime, |runtime| f(&mut runtime.runtime, id))
}

pub(crate) fn with_patch_ref<T>(
    patch: SnowPatchHandle,
    f: impl FnOnce(&SnowPatchHandleImpl) -> Result<T, SnowError>,
) -> Result<T, SnowError> {
    if patch.is_null() {
        return Err(SnowError::InvalidArgument);
    }
    let patch = unsafe { &*patch };
    f(patch)
}

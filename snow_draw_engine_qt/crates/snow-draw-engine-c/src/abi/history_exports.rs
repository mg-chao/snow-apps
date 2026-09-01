use crate::abi::handles::*;
use crate::abi::types::*;

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `out_state` must be valid for writes of one `SnowHistoryState` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_get_history_state(
    runtime: SnowRuntime,
    out_state: *mut SnowHistoryState,
) -> SnowError {
    ffi_error(|| {
        if out_state.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_ref(runtime, |runtime| {
            write_out(out_state, runtime.history_state().into());
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_clear_document_preserving_viewports(
    runtime: SnowRuntime,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let result = state
                .runtime
                .clear_document_preserving_viewports()
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `bytes` must point to `size` readable bytes containing a serialized document history.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_restore_document_history_preserving_editor_styles(
    runtime: SnowRuntime,
    bytes: *const u8,
    size: usize,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if bytes.is_null() || size == 0 || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let result = state
                .runtime
                .restore_document_history_preserving_editor_styles(bytes)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_undo_ex(
    runtime: SnowRuntime,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let result = state
                .runtime
                .undo_with_viewport_changes()
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_redo_ex(
    runtime: SnowRuntime,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let result = state
                .runtime
                .redo_with_viewport_changes()
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

use crate::abi::{handles::*, types::*};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_document_revision(runtime: SnowRuntime) -> u64 {
    ffi_value(0, || {
        with_runtime_ref(runtime, |r| Ok(r.document_revision())).unwrap_or(0)
    })
}

/// Applies once. The caller owns the returned boxed byte slice and must free it with
/// snow_annotation_result_destroy. No size-query call can accidentally apply twice.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_apply_annotation_json(
    runtime: SnowRuntime,
    bytes: *const u8,
    size: usize,
    out_json: *mut *mut u8,
    out_size: *mut usize,
    out_changed: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_json.is_null() || out_size.is_null() || out_changed.is_null() {
            return SnowError::InvalidArgument;
        }
        write_out(out_json, std::ptr::null_mut());
        write_out(out_size, 0);
        write_out(out_changed, std::ptr::null_mut());
        if bytes.is_null() || size == 0 || size > 1024 * 1024 {
            return SnowError::InvalidArgument;
        }
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let (mutation, result) = state
                .runtime
                .apply_annotation_json(bytes)
                .map_err(SnowError::from)?;
            let mut result = result.into_boxed_slice();
            write_out(out_size, result.len());
            write_out(out_json, result.as_mut_ptr());
            std::mem::forget(result);
            write_changed_viewports(out_changed, mutation.changed_viewports);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_annotation_result_destroy(bytes: *mut u8, size: usize) {
    if !bytes.is_null() {
        ffi_void(|| {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(bytes, size)) });
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::*;
    #[test]
    fn annotation_ffi_applies_once_and_handles_invalid_pointers() {
        unsafe {
            let mut runtime = std::ptr::null_mut();
            assert_eq!(snow_runtime_create(&mut runtime), SnowError::Ok);
            let bytes =
                br#"{"version":1,"operations":[{"type":"rectangle","bounds":[1,2,30,40]}]}"#;
            let mut json = std::ptr::null_mut();
            let mut size = 0;
            let mut changed = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_apply_annotation_json(
                    runtime,
                    bytes.as_ptr(),
                    bytes.len(),
                    &mut json,
                    &mut size,
                    &mut changed
                ),
                SnowError::Ok
            );
            assert!(size > 0);
            snow_annotation_result_destroy(json, size);
            snow_changed_viewports_destroy(changed);
            assert_eq!(
                snow_runtime_apply_annotation_json(
                    runtime,
                    std::ptr::null(),
                    1,
                    &mut json,
                    &mut size,
                    &mut changed
                ),
                SnowError::InvalidArgument
            );
            assert!(json.is_null());
            assert_eq!(size, 0);
            snow_runtime_destroy(runtime);
        }
    }
}

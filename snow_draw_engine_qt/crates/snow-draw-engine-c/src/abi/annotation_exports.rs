use crate::abi::{handles::*, types::*};

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_visit_serial_number_cursor_preview(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    view_x: f64,
    view_y: f64,
    modifiers: SnowModifiers,
    visitor: Option<unsafe extern "C" fn(*mut std::ffi::c_void, *const SnowSceneDisplayItem)>,
    context: *mut std::ffi::c_void,
) -> SnowError {
    ffi_error(|| {
        let Some(visitor) = visitor else {
            return SnowError::InvalidArgument;
        };
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let item = runtime
                    .viewport_serial_number_cursor_preview(
                        id,
                        snow_draw_engine::Point::new(view_x, view_y),
                        crate::abi::convert::snow_modifiers_to_rust(modifiers),
                    )
                    .map_err(SnowError::from)?;
                let converted =
                    crate::abi::convert::snow_scene_display_item_from_rust(&item, false, false);
                unsafe {
                    visitor(context, &converted.view);
                }
                Ok(())
            },
        ))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_document_revision(runtime: SnowRuntime) -> u64 {
    ffi_value(0, || {
        with_runtime_ref(runtime, |r| Ok(r.document_revision())).unwrap_or(0)
    })
}

/// # Safety
/// Runtime must be a live handle or null. Null returns zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_angle_adjustment_target_revision(
    runtime: SnowRuntime,
) -> u64 {
    ffi_value(0, || {
        with_runtime_ref(runtime, |r| Ok(r.angle_adjustment_target_revision())).unwrap_or(0)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_has_document_content(runtime: SnowRuntime) -> u8 {
    ffi_value(1, || {
        with_runtime_ref(runtime, |r| Ok(u8::from(r.has_document_content()))).unwrap_or(1)
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
    fn serial_number_cursor_visitor_validates_inputs_and_borrows_complete_item() {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            item: *const SnowSceneDisplayItem,
        ) {
            let item = unsafe { &*item };
            assert_eq!(item.kind, SnowSceneDisplayItemKind::SerialNumber);
            assert!(item.width > 0.0 && item.font_size > 0.0);
            assert!(item.text_utf8_len > 0 && !item.text_utf8.is_null());
            unsafe {
                *context.cast::<u32>() += 1;
            }
        }
        unsafe {
            let mut runtime = std::ptr::null_mut();
            let mut viewport = std::ptr::null_mut();
            assert_eq!(snow_runtime_create(&mut runtime), SnowError::Ok);
            let config = SnowEngineConfig {
                min_zoom: 0.1,
                max_zoom: 8.0,
                zoom_focus: SnowZoomFocus::Pointer,
                wheel_zoom_sensitivity: 0.001,
                clear_color: SnowColorRgba8::default(),
                snap: SnowSnapConfig::default(),
                grid: SnowGridConfig::default(),
                enable_pointer_capture: 0,
                reserved: [0; 7],
            };
            assert_eq!(
                snow_viewport_create(runtime, &config, &mut viewport),
                SnowError::Ok
            );
            let mut calls = 0u32;
            let context = (&mut calls as *mut u32).cast();
            for (runtime_arg, viewport_arg, x, visitor) in [
                (
                    runtime,
                    viewport,
                    10.0,
                    Some(visit as unsafe extern "C" fn(_, _)),
                ),
                (
                    std::ptr::null_mut(),
                    viewport,
                    10.0,
                    Some(visit as unsafe extern "C" fn(_, _)),
                ),
                (
                    runtime,
                    std::ptr::null_mut(),
                    10.0,
                    Some(visit as unsafe extern "C" fn(_, _)),
                ),
                (
                    runtime,
                    viewport,
                    f64::NAN,
                    Some(visit as unsafe extern "C" fn(_, _)),
                ),
                (runtime, viewport, 10.0, None),
            ] {
                let expected = if calls == 0
                    && !runtime_arg.is_null()
                    && !viewport_arg.is_null()
                    && x.is_finite()
                    && visitor.is_some()
                {
                    SnowError::Ok
                } else {
                    SnowError::InvalidArgument
                };
                assert_eq!(
                    snow_viewport_visit_serial_number_cursor_preview(
                        runtime_arg,
                        viewport_arg,
                        x,
                        10.0,
                        SnowModifiers::default(),
                        visitor,
                        context
                    ),
                    expected
                );
            }
            assert_eq!(calls, 1);
            snow_viewport_destroy(runtime, viewport);
            snow_runtime_destroy(runtime);
        }
    }

    #[test]
    fn annotation_ffi_applies_once_and_handles_invalid_pointers() {
        unsafe {
            let mut runtime = std::ptr::null_mut();
            assert_eq!(snow_runtime_create(&mut runtime), SnowError::Ok);
            assert_eq!(snow_runtime_has_document_content(runtime), 0);
            assert_eq!(snow_runtime_has_document_content(std::ptr::null_mut()), 1);
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
            assert_eq!(snow_runtime_has_document_content(runtime), 1);
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

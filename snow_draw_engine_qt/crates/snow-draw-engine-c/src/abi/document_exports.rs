use snow_draw_engine::{ElementId, Point};

use crate::abi::handles::*;
use crate::abi::types::*;

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_reset_editing_state_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let viewport_id = viewport_id(viewport)?;
            let result = state
                .runtime
                .reset_editing_state_with_viewport_changes(viewport_id)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_selected` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_is_element_selected(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    id: SnowElementId,
    out_selected: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_selected.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                let selected = runtime.selected_ids().contains(&ElementId {
                    index: id.index,
                    generation: id.generation,
                });
                write_out(out_selected, u8::from(selected));
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_select_element_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    id: SnowElementId,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let viewport_id = viewport_id(viewport)?;
            let result = state
                .runtime
                .select_element_with_viewport_changes(
                    viewport_id,
                    ElementId {
                        index: id.index,
                        generation: id.generation,
                    },
                )
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_delete_selected_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .delete_selected_with_viewport_changes(id)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_delete_all_elements_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .delete_all_elements_with_viewport_changes(id)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_duplicate_selected_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    offset_x: f64,
    offset_y: f64,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .duplicate_selected_with_viewport_changes(id, Point::new(offset_x, offset_y))
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_reorder_selected_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    action: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .reorder_selected_with_viewport_changes(id, action)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_selected_opacity_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    opacity: f64,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_selected_opacity_with_viewport_changes(id, opacity)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_adjust_selected_serial_numbers_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    delta: i64,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .adjust_selected_serial_numbers_with_viewport_changes(id, delta)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// Visits borrowed, uncropped Smart Erase geometry. The visitor must copy data and must not reenter the runtime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_visit_smart_erase(
    runtime: SnowRuntime,
    visitor: Option<unsafe extern "C" fn(*mut std::ffi::c_void, *const SnowSceneDisplayItem)>,
    context: *mut std::ffi::c_void,
) -> SnowError {
    ffi_error(|| {
        let Some(visitor) = visitor else {
            return SnowError::InvalidArgument;
        };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            for item in state.runtime.smart_erase_items() {
                let converted =
                    crate::abi::convert::snow_scene_display_item_from_rust(&item, false, false);
                unsafe {
                    visitor(context, &converted.view);
                }
            }
            Ok(())
        }))
    })
}

use snow_draw_engine::{Point, Runtime, ViewportConfig};

use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::types::*;

/// # Safety
/// If `out_runtime` is non-null, it must be valid for writes of one `SnowRuntime` value.
/// The returned handle must later be released with `snow_runtime_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create(out_runtime: *mut SnowRuntime) -> SnowError {
    unsafe { snow_runtime_create_with_config(std::ptr::null(), out_runtime) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create_with_config(
    config: *const SnowRuntimeConfig,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let config = match runtime_config_from_c(unsafe { config.as_ref() }) {
            Ok(config) => config,
            Err(error) => return error,
        };
        match Runtime::try_new(config) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_style_defaults_default(
    out_defaults: *mut SnowStyleDefaults,
) -> SnowError {
    ffi_error(|| {
        if out_defaults.is_null() {
            return SnowError::InvalidArgument;
        }
        write_out(
            out_defaults,
            snow_draw_engine::StyleDefaults::default().into(),
        );
        SnowError::Ok
    })
}

/// # Safety
/// If `source` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// If `out_runtime` is non-null, it must be valid for writes of one `SnowRuntime` value.
/// The returned handle must later be released with `snow_runtime_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_clone_document_session(
    source: SnowRuntime,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if source.is_null() || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }

        let source = unsafe { &*source };
        write_out(
            out_runtime,
            Box::into_raw(Box::new(SnowRuntimeImpl {
                runtime: source.runtime.clone_document_session(),
            })),
        );
        SnowError::Ok
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_clone_document_session_with_config(
    source: SnowRuntime,
    config: *const SnowRuntimeConfig,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if source.is_null() || config.is_null() || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let config = match runtime_config_from_c(unsafe { config.as_ref() }) {
            Ok(config) => config,
            Err(error) => return error,
        };
        let source = unsafe { &*source };
        match source.runtime.clone_document_session_with_config(config) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

/// Serializes the document/editor/history session. Call with a null buffer and zero
/// capacity to query the required byte count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_serialize_document_session(
    runtime: SnowRuntime,
    buffer: *mut u8,
    buffer_capacity: usize,
    out_size: *mut usize,
) -> SnowError {
    ffi_error(|| {
        if runtime.is_null() || out_size.is_null() || (buffer.is_null() && buffer_capacity != 0) {
            return SnowError::InvalidArgument;
        }
        let runtime = unsafe { &*runtime };
        let bytes = match runtime.runtime.serialize_document_session() {
            Ok(bytes) => bytes,
            Err(error) => return SnowError::from(error),
        };
        write_out(out_size, bytes.len());
        if buffer.is_null() {
            return SnowError::Ok;
        }
        if buffer_capacity < bytes.len() {
            return SnowError::BufferTooSmall;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, bytes.len());
        }
        SnowError::Ok
    })
}

/// Constructs a new runtime from validated session bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create_from_document_session(
    bytes: *const u8,
    size: usize,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if bytes.is_null() || size == 0 || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };
        match Runtime::from_serialized_document_session(bytes) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create_from_document_session_with_config(
    bytes: *const u8,
    size: usize,
    config: *const SnowRuntimeConfig,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if bytes.is_null() || size == 0 || config.is_null() || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let config = match runtime_config_from_c(unsafe { config.as_ref() }) {
            Ok(config) => config,
            Err(error) => return error,
        };
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };
        match Runtime::from_serialized_document_session_with_config(bytes, config) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

/// Serializes only the document elements and undo/redo history. Call with a
/// null buffer and zero capacity to query the required byte count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_serialize_document_history(
    runtime: SnowRuntime,
    buffer: *mut u8,
    buffer_capacity: usize,
    out_size: *mut usize,
) -> SnowError {
    ffi_error(|| {
        if runtime.is_null() || out_size.is_null() || (buffer.is_null() && buffer_capacity != 0) {
            return SnowError::InvalidArgument;
        }
        let runtime = unsafe { &*runtime };
        let bytes = match runtime.runtime.serialize_document_history() {
            Ok(bytes) => bytes,
            Err(error) => return SnowError::from(error),
        };
        write_out(out_size, bytes.len());
        if buffer.is_null() {
            return SnowError::Ok;
        }
        if buffer_capacity < bytes.len() {
            return SnowError::BufferTooSmall;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, bytes.len());
        }
        SnowError::Ok
    })
}

/// Constructs a new runtime from validated document-history bytes. Editor and
/// session configuration state are initialized to their defaults.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create_from_document_history(
    bytes: *const u8,
    size: usize,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if bytes.is_null() || size == 0 || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };
        match Runtime::from_serialized_document_history(bytes) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_create_from_document_history_with_config(
    bytes: *const u8,
    size: usize,
    config: *const SnowRuntimeConfig,
    out_runtime: *mut SnowRuntime,
) -> SnowError {
    ffi_error(|| {
        if bytes.is_null() || size == 0 || config.is_null() || out_runtime.is_null() {
            return SnowError::InvalidArgument;
        }
        let config = match runtime_config_from_c(unsafe { config.as_ref() }) {
            Ok(config) => config,
            Err(error) => return error,
        };
        let bytes = unsafe { std::slice::from_raw_parts(bytes, size) };
        match Runtime::from_serialized_document_history_with_config(bytes, config) {
            Ok(runtime) => {
                write_out(
                    out_runtime,
                    Box::into_raw(Box::new(SnowRuntimeImpl { runtime })),
                );
                SnowError::Ok
            }
            Err(error) => SnowError::from(error),
        }
    })
}

/// # Safety
/// If `out_engine` is non-null, it must be valid for writes of one `SnowEngine` value.
/// The returned handle must later be released with `snow_engine_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_engine_create(out_engine: *mut SnowEngine) -> SnowError {
    ffi_error(|| unsafe { snow_runtime_create(out_engine.cast()) })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// The handle must not be used again after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_destroy(runtime: SnowRuntime) {
    ffi_void(|| {
        if runtime.is_null() {
            return;
        }
        unsafe {
            drop(Box::from_raw(runtime));
        }
    })
}

/// # Safety
/// If `engine` is non-null, it must be a live handle returned by `snow_engine_create`.
/// The handle must not be used again after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_engine_destroy(engine: SnowEngine) {
    ffi_void(|| unsafe { snow_runtime_destroy(engine) })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_set_quick_selection_disabled_tools(
    runtime: SnowRuntime,
    tools: u64,
) -> SnowError {
    ffi_error(|| {
        let mut changed_viewports = std::ptr::null_mut();
        let error = unsafe {
            snow_runtime_set_quick_selection_disabled_tools_ex(
                runtime,
                tools,
                &mut changed_viewports,
            )
        };
        unsafe {
            snow_changed_viewports_destroy(changed_viewports);
        }
        error
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by `snow_runtime_create`.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_set_quick_selection_disabled_tools_ex(
    runtime: SnowRuntime,
    tools: u64,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let result = state
                .runtime
                .set_quick_selection_disabled_tools(snow_active_tool_mask_to_rust(tools))
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live `SnowRuntime` handle.
/// `config` must point to a readable `SnowEngineConfig` and `out_viewport` must be writable.
/// The returned viewport handle must later be released with `snow_viewport_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_create(
    runtime: SnowRuntime,
    config: *const SnowEngineConfig,
    out_viewport: *mut SnowViewport,
) -> SnowError {
    ffi_error(|| {
        if config.is_null() || out_viewport.is_null() {
            return SnowError::InvalidArgument;
        }

        let engine = snow_engine_config_to_rust(unsafe { &*config });

        match with_runtime_impl_mut(runtime, |state| {
            let id = state
                .runtime
                .create_viewport(ViewportConfig { engine })
                .map_err(SnowError::from)?;
            write_out(
                out_viewport,
                Box::into_raw(Box::new(SnowViewportImpl { id })),
            );
            Ok(())
        }) {
            Ok(()) => SnowError::Ok,
            Err(error) => error,
        }
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live `SnowRuntime` handle.
/// If `viewport` is non-null, it must be a live handle created by `snow_viewport_create`.
/// The viewport handle must not be used again after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_destroy(runtime: SnowRuntime, viewport: SnowViewport) {
    ffi_void(|| {
        if viewport.is_null() {
            return;
        }

        let id = unsafe { (*viewport).id };
        if !runtime.is_null() {
            let _ = with_runtime_impl_mut(runtime, |state| {
                state
                    .runtime
                    .destroy_viewport(id)
                    .map_err(SnowError::from)?;
                Ok(())
            });
        }

        unsafe {
            drop(Box::from_raw(viewport));
        }
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_surface_size(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    width: u32,
    height: u32,
) -> SnowError {
    ffi_error(|| {
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |runtime, id| {
                runtime
                    .set_viewport_surface_size(id, width, height)
                    .map(|_| ())
                    .map_err(SnowError::from)
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_camera(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    center_x: f64,
    center_y: f64,
    zoom: f64,
) -> SnowError {
    ffi_error(|| {
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |runtime, id| {
                runtime
                    .set_viewport_camera(
                        id,
                        snow_draw_engine::Camera {
                            center: Point {
                                x: center_x,
                                y: center_y,
                            },
                            zoom,
                        },
                    )
                    .map(|_| ())
                    .map_err(SnowError::from)
            },
        ))
    })
}

/// # Safety
/// If `viewport` is non-null, it must be a live handle created by this library.
/// `out_id` must be valid for writes of one `uint64_t` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_id(
    viewport: SnowViewport,
    out_id: *mut u64,
) -> SnowError {
    ffi_error(|| {
        if out_id.is_null() {
            return SnowError::InvalidArgument;
        }
        match viewport_id(viewport) {
            Ok(id) => {
                write_out(out_id, id.0);
                SnowError::Ok
            }
            Err(error) => error,
        }
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_active_tool(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    tool: SnowActiveTool,
) -> SnowError {
    ffi_error(|| {
        let mut changed_viewports = std::ptr::null_mut();
        let error = unsafe {
            snow_viewport_set_active_tool_ex(runtime, viewport, tool, &mut changed_viewports)
        };
        unsafe {
            snow_changed_viewports_destroy(changed_viewports);
        }
        error
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_active_tool_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    tool: SnowActiveTool,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |runtime, id| {
                let result = runtime
                    .set_viewport_active_tool(id, snow_active_tool_to_rust(tool))
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed_viewports, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_tool` must be valid for writes of one `SnowActiveTool` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_active_tool(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_tool: *mut SnowActiveTool,
) -> SnowError {
    ffi_error(|| {
        if out_tool.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let tool = runtime.viewport_active_tool(id).map_err(SnowError::from)?;
                write_out(out_tool, snow_active_tool_from_rust(tool));
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_config` must be valid for writes of one `SnowSnapConfig` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_snap_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_config: *mut SnowSnapConfig,
) -> SnowError {
    ffi_error(|| {
        if out_config.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                write_out(
                    out_config,
                    runtime
                        .viewport_snap_config(id)
                        .map_err(SnowError::from)?
                        .into(),
                );
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `config` must point to a readable `SnowSnapConfig` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_snap_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowSnapConfig,
) -> SnowError {
    ffi_error(|| {
        let mut changed_viewports = std::ptr::null_mut();
        let error = unsafe {
            snow_viewport_set_snap_config_ex(runtime, viewport, config, &mut changed_viewports)
        };
        unsafe {
            snow_changed_viewports_destroy(changed_viewports);
        }
        error
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `config` must point to a readable `SnowSnapConfig` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_snap_config_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowSnapConfig,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if config.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |runtime, id| {
                let result = runtime
                    .set_viewport_snap_config(id, unsafe { (*config).into() })
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed_viewports, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_config` must be valid for writes of one `SnowGridConfig` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_grid_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_config: *mut SnowGridConfig,
) -> SnowError {
    ffi_error(|| {
        if out_config.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                write_out(
                    out_config,
                    runtime
                        .viewport_grid_config(id)
                        .map_err(SnowError::from)?
                        .into(),
                );
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `config` must point to a readable `SnowGridConfig` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_grid_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowGridConfig,
) -> SnowError {
    ffi_error(|| {
        let mut changed_viewports = std::ptr::null_mut();
        let error = unsafe {
            snow_viewport_set_grid_config_ex(runtime, viewport, config, &mut changed_viewports)
        };
        unsafe {
            snow_changed_viewports_destroy(changed_viewports);
        }
        error
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `config` must point to a readable `SnowGridConfig` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_grid_config_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowGridConfig,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if config.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |runtime, id| {
                let result = runtime
                    .set_viewport_grid_config(id, unsafe { (*config).into() })
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed_viewports, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `changed_viewports` is non-null, it must be a live handle returned by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_changed_viewports_destroy(
    changed_viewports: SnowChangedViewportList,
) {
    ffi_void(|| {
        if !changed_viewports.is_null() {
            drop(unsafe { Box::from_raw(changed_viewports) });
        }
    })
}

/// # Safety
/// If `changed_viewports` is non-null, it must be a live handle returned by this library.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_changed_viewports_count(
    changed_viewports: SnowChangedViewportList,
) -> u32 {
    ffi_value(0, || {
        if changed_viewports.is_null() {
            return 0;
        }
        let changed_viewports = unsafe { &*changed_viewports };
        changed_viewports.viewport_ids.len().min(u32::MAX as usize) as u32
    })
}

/// # Safety
/// If `changed_viewports` is non-null, it must be a live handle returned by this library.
/// `out_viewport_id` must be valid for writes of one `uint64_t` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_changed_viewports_get(
    changed_viewports: SnowChangedViewportList,
    index: u32,
    out_viewport_id: *mut u64,
) -> SnowError {
    ffi_error(|| {
        if changed_viewports.is_null() || out_viewport_id.is_null() {
            return SnowError::InvalidArgument;
        }
        let changed_viewports = unsafe { &*changed_viewports };
        let Some(viewport_id) = changed_viewports.viewport_ids.get(index as usize) else {
            return SnowError::NotFound;
        };
        write_out(out_viewport_id, *viewport_id);
        SnowError::Ok
    })
}

#[cfg(test)]
mod session_tests {
    use super::*;
    use snow_draw_engine::ActiveTool;

    #[test]
    fn session_abi_is_two_pass_and_rejects_bad_input() {
        unsafe {
            let mut runtime = std::ptr::null_mut();
            assert_eq!(snow_runtime_create(&mut runtime), SnowError::Ok);

            let mut required = 0usize;
            assert_eq!(
                snow_runtime_serialize_document_session(
                    runtime,
                    std::ptr::null_mut(),
                    0,
                    &mut required,
                ),
                SnowError::Ok
            );
            assert!(required > 0);

            let mut too_small = vec![0u8; required - 1];
            let mut reported = 0usize;
            assert_eq!(
                snow_runtime_serialize_document_session(
                    runtime,
                    too_small.as_mut_ptr(),
                    too_small.len(),
                    &mut reported,
                ),
                SnowError::BufferTooSmall
            );
            assert_eq!(reported, required);

            let mut payload = vec![0u8; required];
            assert_eq!(
                snow_runtime_serialize_document_session(
                    runtime,
                    payload.as_mut_ptr(),
                    payload.len(),
                    &mut reported,
                ),
                SnowError::Ok
            );
            let mut restored = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_create_from_document_session(
                    payload.as_ptr(),
                    payload.len(),
                    &mut restored,
                ),
                SnowError::Ok
            );
            assert!(!restored.is_null());

            let mut history_required = 0usize;
            assert_eq!(
                snow_runtime_serialize_document_history(
                    runtime,
                    std::ptr::null_mut(),
                    0,
                    &mut history_required,
                ),
                SnowError::Ok
            );
            assert!(history_required > 0);
            let mut history_payload = vec![0u8; history_required];
            assert_eq!(
                snow_runtime_serialize_document_history(
                    runtime,
                    history_payload.as_mut_ptr(),
                    history_payload.len(),
                    &mut history_required,
                ),
                SnowError::Ok
            );
            let mut history_restored = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_create_from_document_history(
                    history_payload.as_ptr(),
                    history_payload.len(),
                    &mut history_restored,
                ),
                SnowError::Ok
            );
            assert!(!history_restored.is_null());

            let malformed = br#"{"schemaVersion":999}"#;
            let mut rejected = std::ptr::null_mut();
            assert_ne!(
                snow_runtime_create_from_document_session(
                    malformed.as_ptr(),
                    malformed.len(),
                    &mut rejected,
                ),
                SnowError::Ok
            );
            assert!(rejected.is_null());

            snow_runtime_destroy(restored);
            snow_runtime_destroy(history_restored);
            snow_runtime_destroy(runtime);
        }
    }

    #[test]
    fn configured_runtime_creation_is_atomic() {
        unsafe {
            let mut defaults: SnowStyleDefaults = snow_draw_engine::StyleDefaults::default().into();
            assert_eq!(
                snow_runtime_style_defaults_default(&mut defaults),
                SnowError::Ok
            );
            defaults.text.font_size = 37.0;
            defaults.rectangle_filter.stroke_width = 0.0;
            defaults.pen_filter.stroke_width = 31.0;
            let config = SnowRuntimeConfig {
                style_defaults: &defaults,
            };
            let mut runtime = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_create_with_config(&config, &mut runtime),
                SnowError::Ok
            );
            assert!(!runtime.is_null());
            snow_runtime_destroy(runtime);

            defaults.spotlight.opacity = 1.5;
            let invalid_config = SnowRuntimeConfig {
                style_defaults: &defaults,
            };
            let mut rejected = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_create_with_config(&invalid_config, &mut rejected),
                SnowError::InvalidArgument
            );
            assert!(rejected.is_null());
        }
    }

    #[test]
    fn configured_runtime_creation_rejects_invalid_raw_enum() {
        unsafe {
            let mut defaults = Box::<SnowStyleDefaults>::new_uninit();
            let defaults_ptr = defaults.as_mut_ptr();
            defaults_ptr.write(snow_draw_engine::StyleDefaults::default().into());
            std::ptr::addr_of_mut!((*defaults_ptr).arrow.start_arrowhead)
                .cast::<i32>()
                .write_unaligned(99);

            let config = SnowRuntimeConfig {
                style_defaults: defaults_ptr,
            };
            let mut runtime = std::ptr::null_mut();
            assert_eq!(
                snow_runtime_create_with_config(&config, &mut runtime),
                SnowError::InvalidArgument
            );
            assert!(runtime.is_null());
        }
    }

    #[test]
    fn quick_selection_tool_mask_uses_the_stable_c_enum_layout() {
        let c_mask = (1_u64 << SnowActiveTool::FreeDraw as u32)
            | (1_u64 << SnowActiveTool::PenFilter as u32);
        let rust_mask = snow_active_tool_mask_to_rust(c_mask);

        assert_eq!(
            rust_mask,
            ActiveTool::FreeDraw.policy_bit() | ActiveTool::PenFilter.policy_bit()
        );
        assert_eq!(rust_mask & ActiveTool::Text.policy_bit(), 0);
        assert_eq!(rust_mask & ActiveTool::Spotlight.policy_bit(), 0);
    }
}

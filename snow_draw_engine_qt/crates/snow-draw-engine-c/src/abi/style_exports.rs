use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::types::*;

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_watermark_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_config: *mut SnowWatermarkConfig,
) -> SnowError {
    ffi_error(|| {
        if out_config.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                write_out(out_config, runtime.watermark_config().clone().into());
                Ok(())
            },
        ))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_watermark_config_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowWatermarkConfig,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if config.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_watermark_config(id, unsafe { (*config).into() })
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_spotlight_config(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_config: *mut SnowSpotlightConfig,
) -> SnowError {
    ffi_error(|| {
        if out_config.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                write_out(out_config, runtime.spotlight_config().into());
                Ok(())
            },
        ))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_spotlight_config_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    config: *const SnowSpotlightConfig,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if config.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_spotlight_config(id, unsafe { (*config).into() })
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[cfg(test)]
#[allow(clippy::items_after_test_module)]
mod spotlight_export_tests {
    use super::*;

    #[test]
    fn spotlight_exports_reject_null_output_and_config_pointers() {
        unsafe {
            assert_eq!(
                snow_viewport_get_spotlight_config(
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_viewport_set_spotlight_config_ex(
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    std::ptr::null(),
                    std::ptr::null_mut(),
                ),
                SnowError::InvalidArgument
            );
            let config = SnowSpotlightConfig::default();
            assert_eq!(
                snow_viewport_set_spotlight_config_ex(
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &config,
                    std::ptr::null_mut(),
                ),
                SnowError::InvalidArgument
            );
        }
    }
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_state` must be valid for writes of one `SnowStyleToolbarState` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_style_toolbar_state(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_state: *mut SnowStyleToolbarState,
) -> SnowError {
    ffi_error(|| {
        if out_state.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let state = runtime
                    .viewport_style_toolbar_state(id)
                    .map_err(SnowError::from)?;
                write_out(
                    out_state,
                    SnowStyleToolbarState {
                        source: snow_style_toolbar_source_from_rust(state.source),
                        reserved0: 0,
                        shape_style: state.shape_style.into(),
                        text_style: state.text_style.into(),
                        serial_number_style: state.serial_number_style.into(),
                        text_style_mixed: state.text_style_mixed,
                        serial_number_style_mixed: state.serial_number_style_mixed,
                        shape_style_mixed: state.shape_style_mixed,
                        filter_style: state.filter_style.into(),
                        filter_style_mixed: state.filter_style_mixed,
                    },
                );
                Ok(())
            },
        ))
    })
}

/// # Safety
/// Handles and output pointers must be live and writable; `style` must be readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_filter_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowFilterStyle,
    properties: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_filter_style(id, unsafe { (*style).into() }, properties)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_state` must be valid for writes of one `SnowSerialNumberToolbarState` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_serial_number_toolbar_state(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_state: *mut SnowSerialNumberToolbarState,
) -> SnowError {
    ffi_error(|| {
        if out_state.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let state = runtime
                    .viewport_serial_number_toolbar_state(id)
                    .map_err(SnowError::from)?;
                write_out(
                    out_state,
                    SnowSerialNumberToolbarState {
                        visible: u8::from(state.visible),
                        can_decrease: u8::from(state.can_decrease),
                        can_increase: u8::from(state.can_increase),
                        can_create_text: u8::from(state.can_create_text),
                        reserved0: [0; 4],
                        left: state.left,
                        top: state.top,
                        width: state.width,
                        height: state.height,
                    },
                );
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `style` must point to a readable `SnowShapeStyle` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_shape_style_patch_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowShapeStyle,
    properties: u32,
    kind: SnowShapeKind,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_shape_style_patch(
                    id,
                    snow_draw_engine::ShapeStylePatch {
                        kind: snow_shape_kind_to_rust(kind),
                        style: unsafe { (*style).into() },
                        properties,
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
/// `style` must point to a readable `SnowRectangleShapeStyle` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_rectangle_shape_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowRectangleShapeStyle,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_rectangle_shape_style(id, unsafe { (*style).into() })
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `style` must point to a readable `SnowTextStyle` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_text_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowTextStyle,
    layouts: *const SnowTextLayoutOverride,
    layout_count: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null()
            || out_changed_viewports.is_null()
            || (layouts.is_null() && layout_count != 0)
        {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let layout_slice = if layout_count == 0 {
                &[][..]
            } else {
                unsafe { std::slice::from_raw_parts(layouts, layout_count as usize) }
            };
            let layout_overrides = layout_slice
                .iter()
                .copied()
                .map(Into::into)
                .collect::<Vec<_>>();
            let result = state
                .runtime
                .set_viewport_text_style(id, unsafe { (*style).into() }, &layout_overrides)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `style` must point to a readable `SnowSerialNumberStyle` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_serial_number_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowSerialNumberStyle,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_serial_number_style(id, unsafe { (*style).into() })
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

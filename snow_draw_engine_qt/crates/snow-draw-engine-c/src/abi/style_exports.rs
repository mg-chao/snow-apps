use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::raw_enum::SnowRawEnum;
use crate::abi::types::*;

/// # Safety
/// Handles must be live and pointers must be readable/writable as indicated.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_distance_style_patch_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowDistanceStyle,
    properties: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        let valid = unsafe {
            raw_c_enum_is_valid(std::ptr::addr_of!((*style).unit))
                && raw_c_enum_is_valid(std::ptr::addr_of!((*style).endpoint_style))
                && (*style).decimal_places <= 3
        };
        if !valid {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_distance_style_patch(id, unsafe { (*style).into() }, properties)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// Handles must be live and output must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_distance_pixel_scale_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    scale_x: f64,
    scale_y: f64,
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
                .set_viewport_distance_pixel_scale(
                    id,
                    snow_draw_engine::Point::new(scale_x, scale_y),
                )
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

unsafe fn filter_style_type_is_valid(style: *const SnowFilterStyle) -> bool {
    let raw =
        unsafe { std::ptr::read_unaligned(std::ptr::addr_of!((*style).filter_type).cast::<i32>()) };
    SnowFilterType::from_raw(raw).is_some()
}

unsafe fn serial_number_style_type_is_valid(style: *const SnowSerialNumberStyle) -> bool {
    let raw = unsafe {
        std::ptr::read_unaligned(std::ptr::addr_of!((*style).serial_number_type).cast::<i32>())
    };
    let numeric_raw = unsafe {
        std::ptr::read_unaligned(std::ptr::addr_of!((*style).numeric_type).cast::<i32>())
    };
    SnowSerialNumberType::from_raw(raw).is_some()
        && SnowSerialNumberNumericType::from_raw(numeric_raw).is_some()
}

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
                        selected_element_count: state.selected_element_count,
                        shape_style: state.shape_style.into(),
                        text_style: state.text_style.into(),
                        serial_number_style: state.serial_number_style.into(),
                        text_style_mixed: state.text_style_mixed,
                        serial_number_style_mixed: state.serial_number_style_mixed,
                        shape_style_mixed: state.shape_style_mixed,
                        filter_style: state.filter_style.into(),
                        filter_style_mixed: state.filter_style_mixed,
                        brush_eraser_style: state.brush_eraser_style.into(),
                        distance_style: state.distance_style.into(),
                        distance_style_mixed: state.distance_style_mixed,
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
        if style.is_null()
            || out_changed_viewports.is_null()
            || !unsafe { filter_style_type_is_valid(style) }
        {
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
/// Handles and output pointers must be live and writable; `style` must be readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_filter_creation_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowFilterStyle,
    properties: u32,
    // Decode the C enum as an integer so invalid discriminants can be rejected safely.
    tool: i32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null()
            || out_changed_viewports.is_null()
            || !unsafe { filter_style_type_is_valid(style) }
        {
            return SnowError::InvalidArgument;
        }
        let tool = match tool {
            value if value == SnowActiveTool::RectangleFilter as i32 => {
                snow_draw_engine::ActiveTool::RectangleFilter
            }
            value if value == SnowActiveTool::PenFilter as i32 => {
                snow_draw_engine::ActiveTool::PenFilter
            }
            _ => return SnowError::InvalidArgument,
        };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_filter_creation_style(
                    id,
                    unsafe { (*style).into() },
                    properties,
                    tool,
                )
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[cfg(test)]
mod filter_creation_style_tests {
    use super::*;

    #[test]
    fn filter_creation_style_export_preserves_state_and_output_on_rejection() {
        let mut state = SnowRuntimeImpl {
            runtime: snow_draw_engine::Runtime::new(snow_draw_engine::RuntimeConfig::default()),
        };
        let id = state
            .runtime
            .create_viewport(snow_draw_engine::ViewportConfig::default())
            .unwrap();
        let mut viewport = SnowViewportImpl { id };
        state
            .runtime
            .set_viewport_active_tool(id, snow_draw_engine::ActiveTool::Select)
            .unwrap();
        let before = state.runtime.serialize_document_session().unwrap();
        let style = SnowFilterStyle {
            filter_type: SnowFilterType::GaussianBlur,
            strength: 0.3,
            opacity: 0.6,
            stroke_width: 20.0,
        };
        let sentinel = std::ptr::dangling_mut::<SnowChangedViewportListImpl>();
        let mut changed = sentinel;
        unsafe {
            let mut invalid = Box::<SnowFilterStyle>::new_uninit();
            let invalid_ptr = invalid.as_mut_ptr();
            invalid_ptr.write(style);
            std::ptr::addr_of_mut!((*invalid_ptr).filter_type)
                .cast::<i32>()
                .write_unaligned(99);
            for (patch, properties, tool) in [
                (
                    &style as *const _,
                    u32::MAX,
                    SnowActiveTool::PenFilter as i32,
                ),
                (
                    &style as *const _,
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    SnowActiveTool::Shape as i32,
                ),
                (
                    std::ptr::null(),
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    SnowActiveTool::PenFilter as i32,
                ),
                (
                    invalid_ptr.cast_const(),
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    SnowActiveTool::PenFilter as i32,
                ),
                (
                    &style as *const _,
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    99,
                ),
            ] {
                assert_eq!(
                    snow_viewport_set_filter_creation_style_ex(
                        &mut state,
                        &mut viewport,
                        patch,
                        properties,
                        tool,
                        &mut changed
                    ),
                    SnowError::InvalidArgument
                );
                assert_eq!(changed, sentinel);
                assert_eq!(state.runtime.serialize_document_session().unwrap(), before);
            }
            assert_eq!(
                snow_viewport_set_filter_style_ex(
                    &mut state,
                    &mut viewport,
                    invalid_ptr,
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    &mut changed
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(changed, sentinel);
            assert_eq!(state.runtime.serialize_document_session().unwrap(), before);
            changed = std::ptr::null_mut();
            assert_eq!(
                snow_viewport_set_filter_creation_style_ex(
                    &mut state,
                    &mut viewport,
                    &style,
                    snow_draw_engine::FILTER_STYLE_PROPERTY_ALL,
                    SnowActiveTool::PenFilter as i32,
                    &mut changed
                ),
                SnowError::Ok
            );
            assert!(!changed.is_null());
            assert_eq!(
                state.runtime.viewport_active_tool(id).unwrap(),
                snow_draw_engine::ActiveTool::Select
            );
            assert!(!state.runtime.history_state().can_undo);
            crate::abi::exports::snow_changed_viewports_destroy(changed);
        }
    }
}

#[cfg(test)]
mod serial_number_style_type_tests {
    use super::*;

    #[test]
    fn invalid_raw_type_is_rejected_before_style_conversion() {
        unsafe {
            let valid = SnowSerialNumberStyle::default();
            assert!(serial_number_style_type_is_valid(&valid));

            let mut invalid = Box::<SnowSerialNumberStyle>::new_uninit();
            let invalid_ptr = invalid.as_mut_ptr();
            invalid_ptr.write(SnowSerialNumberStyle::default());
            std::ptr::addr_of_mut!((*invalid_ptr).serial_number_type)
                .cast::<i32>()
                .write_unaligned(99);
            assert!(!serial_number_style_type_is_valid(invalid_ptr));
            invalid_ptr.write(SnowSerialNumberStyle::default());
            std::ptr::addr_of_mut!((*invalid_ptr).numeric_type)
                .cast::<i32>()
                .write_unaligned(99);
            assert!(!serial_number_style_type_is_valid(invalid_ptr));
        }
    }
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
    unsafe {
        snow_viewport_patch_text_style_ex(
            runtime,
            viewport,
            style,
            snow_draw_engine::TEXT_STYLE_ALL_PROPERTIES,
            layouts,
            layout_count,
            out_changed_viewports,
        )
    }
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `style` must point to a readable `SnowTextStyle` value.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_patch_text_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowTextStyle,
    properties: u32,
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
                .set_viewport_text_style_patch(
                    id,
                    unsafe { (*style).into() },
                    properties,
                    &layout_overrides,
                )
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
        if style.is_null()
            || out_changed_viewports.is_null()
            || !unsafe { serial_number_style_type_is_valid(style) }
        {
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

/// Apply only the specified serial-number properties.
///
/// # Safety
/// Handles and pointers must satisfy the same requirements as the full style setter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_serial_number_style_patch_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowSerialNumberStyle,
    properties: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if style.is_null()
            || out_changed_viewports.is_null()
            || !unsafe { serial_number_style_type_is_valid(style) }
        {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_viewport_serial_number_style_patch(id, unsafe { (*style).into() }, properties)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// Update future text defaults without touching selected objects or an active draft.
///
/// # Safety
/// Handles must be live, style readable and out_changed_viewports writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_text_creation_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowTextStyle,
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
                .set_viewport_text_creation_style(id, unsafe { (*style).into() }, properties)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// Handles must be live; style must be readable and output writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_brush_eraser_creation_style_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    style: *const SnowBrushEraserStyle,
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
                .set_viewport_brush_eraser_creation_style(
                    id,
                    unsafe { (*style).into() },
                    properties,
                )
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

#[cfg(test)]
mod brush_eraser_tests {
    use super::*;

    #[test]
    fn eraser_filters_ffi_style_is_creation_only_and_rejection_preserves_output() {
        let mut state = SnowRuntimeImpl {
            runtime: snow_draw_engine::Runtime::default(),
        };
        let id = state
            .runtime
            .create_viewport(snow_draw_engine::ViewportConfig::default())
            .unwrap();
        let mut viewport = SnowViewportImpl { id };
        state
            .runtime
            .set_viewport_active_tool(id, snow_draw_engine::ActiveTool::BrushEraser)
            .unwrap();
        let before = state.runtime.serialize_document_session().unwrap();
        let sentinel = std::ptr::dangling_mut::<SnowChangedViewportListImpl>();
        let mut changed = sentinel;
        for (style, properties) in [
            (
                SnowBrushEraserStyle { stroke_width: 0.0 },
                SNOW_BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
            ),
            (
                SnowBrushEraserStyle {
                    stroke_width: f64::NAN,
                },
                SNOW_BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
            ),
            (SnowBrushEraserStyle::default(), u32::MAX),
        ] {
            assert_eq!(
                unsafe {
                    snow_viewport_set_brush_eraser_creation_style_ex(
                        &mut state,
                        &mut viewport,
                        &style,
                        properties,
                        &mut changed,
                    )
                },
                SnowError::InvalidArgument
            );
            assert_eq!(changed, sentinel);
            assert_eq!(state.runtime.serialize_document_session().unwrap(), before);
        }
        let style = SnowBrushEraserStyle { stroke_width: 42.0 };
        assert_eq!(
            unsafe {
                snow_viewport_set_brush_eraser_creation_style_ex(
                    &mut state,
                    &mut viewport,
                    &style,
                    SNOW_BRUSH_ERASER_STYLE_PROPERTY_STROKE_WIDTH,
                    &mut changed,
                )
            },
            SnowError::Ok
        );
        let toolbar = state.runtime.viewport_style_toolbar_state(id).unwrap();
        assert_eq!(toolbar.brush_eraser_style.stroke_width, 42.0);
        assert_eq!(
            snow_style_toolbar_source_from_rust(toolbar.source),
            SnowStyleToolbarSource::DefaultBrushEraser
        );
        assert_eq!(
            state.runtime.viewport_active_tool(id).unwrap(),
            snow_draw_engine::ActiveTool::BrushEraser
        );
        assert!(!state.runtime.history_state().can_undo);
        unsafe {
            crate::abi::exports::snow_changed_viewports_destroy(changed);
        }
        for tool in [SnowActiveTool::RectangleEraser, SnowActiveTool::BrushEraser] {
            assert_eq!(
                snow_active_tool_from_rust(snow_active_tool_to_rust(tool)),
                tool
            );
            assert_eq!(
                snow_active_tool_mask_to_rust(1 << tool as u32),
                snow_active_tool_to_rust(tool).policy_bit()
            );
        }
        assert_eq!(std::mem::size_of::<SnowBrushEraserStyle>(), 8);
        assert_eq!(SnowFilterType::RestoreBackground as u32, 7);
        assert_eq!(SnowActiveTool::RectangleEraser as u32, 15);
        assert_eq!(SnowActiveTool::BrushEraser as u32, 16);
    }
}

use snow_draw_engine::{ElementId, Point, TextLayoutSize};

use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::text::{active_text_draft_from_c, text_draft_commit_from_c, text_string_from_raw};
use crate::abi::types::*;

/// # Safety
/// `runtime` must be null or a live runtime handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_arrow_text_count(runtime: SnowRuntime) -> u32 {
    ffi_value(0, || {
        with_runtime_ref(runtime, |engine| Ok(engine.arrow_text_count() as u32)).unwrap_or(0)
    })
}

/// # Safety
/// Handles must be live and `out_items` must hold `capacity` entries when nonzero.
/// `out_count` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_arrow_text_layout_requests(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_items: *mut SnowArrowTextLayoutRequest,
    capacity: u32,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_count.is_null() || (capacity != 0 && out_items.is_null()) {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |engine, id| {
                let requests = engine
                    .arrow_text_layout_requests(id)
                    .map_err(SnowError::from)?;
                write_out(out_count, requests.len() as u32);
                if capacity != 0 {
                    if capacity < requests.len() as u32 {
                        return Err(SnowError::InvalidArgument);
                    }
                    for (index, request) in requests.into_iter().enumerate() {
                        let mut info = engine
                            .text_element_info(request.text_id)
                            .map_err(SnowError::from)?;
                        info.center = request.text.center;
                        let style = snow_draw_engine::TextStyle {
                            color: request.text.color,
                            font_size: request.text.font_size,
                            font_family: request.text.font_family,
                            fill: request.text.fill,
                            fill_style: request.text.fill_style,
                            stroke: request.text.stroke,
                            stroke_width: request.text.stroke_width,
                            corner_radii: request.text.corner_radii,
                            horizontal_align: request.text.horizontal_align,
                            vertical_align: request.text.vertical_align,
                            opacity: request.text.opacity,
                        };
                        write_out(
                            unsafe { out_items.add(index) },
                            SnowArrowTextLayoutRequest {
                                info: snow_text_element_info_from_rust(info),
                                style: style.into(),
                                key: request.key,
                                max_width: request.max_width,
                            },
                        );
                    }
                }
                Ok(())
            },
        ))
    })
}

/// # Safety
/// Handles must be live, `layouts` must hold `count` entries and output must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_apply_arrow_text_layouts_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    layouts: *const SnowArrowTextLayoutResult,
    count: u32,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() || (count != 0 && layouts.is_null()) {
            return SnowError::InvalidArgument;
        }
        let layouts = if count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(layouts, count as usize) }
        };
        let layouts: Vec<_> = layouts
            .iter()
            .map(|r| (snow_element_id_to_rust(r.text_id), r.key, r.size.into()))
            .collect();
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |engine, id| {
                let result = engine
                    .apply_arrow_text_measurements(id, &layouts)
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed_viewports, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

/// Resolve an idle arrow or its label. With `use_point == 0`, require one selected arrow.
/// # Safety
/// Handles must be live and output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_arrow_text_target(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    use_point: u8,
    x: f64,
    y: f64,
    out_info: *mut SnowTextElementInfo,
    out_style: *mut SnowTextStyle,
    out_found: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null()
            || out_style.is_null()
            || out_found.is_null()
            || (use_point != 0 && (!x.is_finite() || !y.is_finite()))
        {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |engine, id| {
                let target = engine
                    .arrow_text_target(id, (use_point != 0).then_some(Point::new(x, y)))
                    .map_err(SnowError::from)?;
                write_out(out_found, u8::from(target.is_some()));
                if let Some((info, style)) = target {
                    write_out(out_info, snow_text_element_info_from_rust(info));
                    write_out(out_style, style.into());
                }
                Ok(())
            },
        ))
    })
}

/// Read complete text without the fixed-size metadata preview's truncation.
/// # Safety
/// Handles must be live; `out_length` must be writable and `buffer` must have `capacity` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_get_text_utf8(
    runtime: SnowRuntime,
    element: SnowElementId,
    buffer: *mut u8,
    capacity: u32,
    out_length: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_length.is_null() || (capacity != 0 && buffer.is_null()) {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_ref(runtime, |state| {
            let info = state
                .text_element_info(snow_element_id_to_rust(element))
                .map_err(SnowError::from)?;
            let length = u32::try_from(info.text.len()).map_err(|_| SnowError::InvalidState)?;
            write_out(out_length, length);
            if capacity != 0 {
                if capacity < length {
                    return Err(SnowError::InvalidArgument);
                }
                unsafe {
                    std::ptr::copy_nonoverlapping(info.text.as_ptr(), buffer, length as usize);
                }
            }
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `text_utf8` must either be null with `text_utf8_len == 0`, or point to `text_utf8_len` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_create_text(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    center_x: f64,
    center_y: f64,
    text_utf8: *const std::ffi::c_char,
    text_utf8_len: u32,
    measured_width: f64,
    measured_height: f64,
) -> SnowError {
    ffi_error(|| {
        let text = match text_string_from_raw(text_utf8, text_utf8_len) {
            Ok(text) => text,
            Err(error) => return error,
        };

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            state
                .runtime
                .create_text_with_viewport_changes(
                    id,
                    Point::new(center_x, center_y),
                    text,
                    TextLayoutSize {
                        width: measured_width,
                        height: measured_height,
                    },
                )
                .map(|_| ())
                .map_err(SnowError::from)?;
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_id` and `out_hit` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_hit_text(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    canvas_x: f64,
    canvas_y: f64,
    out_id: *mut SnowElementId,
    out_hit: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_id.is_null() || out_hit.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                if let Some(text_id) = runtime
                    .hit_text_at(id, Point::new(canvas_x, canvas_y))
                    .map_err(SnowError::from)?
                {
                    write_out(out_id, snow_element_id_from_rust(text_id));
                    write_out(out_hit, 1);
                } else {
                    write_out(out_id, SnowElementId::default());
                    write_out(out_hit, 0);
                }
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_selected_text_count(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                write_out(out_count, runtime.selected_text_count() as u32);
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_items` must be valid for writes of `capacity` items when `capacity > 0`.
/// `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_selected_text_elements(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_items: *mut SnowTextElementInfo,
    capacity: u32,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_count.is_null() || (out_items.is_null() && capacity != 0) {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                let items = runtime.selected_text_elements();
                write_out(out_count, items.len() as u32);
                if capacity != 0 {
                    let out_slice =
                        unsafe { std::slice::from_raw_parts_mut(out_items, capacity as usize) };
                    for (target, item) in out_slice.iter_mut().zip(items) {
                        *target = snow_text_element_info_from_rust(item);
                    }
                }
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_info` and `out_active` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_active_text_resize_measurement(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_info: *mut SnowTextElementInfo,
    out_active: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null() || out_active.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let request = runtime
                    .active_text_resize_measurement_request(id)
                    .map_err(SnowError::from)?;
                write_out(out_active, u8::from(request.is_some()));
                if let Some(info) = request {
                    write_out(out_info, snow_text_element_info_from_rust(info));
                }
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `layout` must point to a readable `SnowTextLayoutSize`, and `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_apply_active_text_resize_measurement_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    layout: *const SnowTextLayoutSize,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if layout.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .apply_active_text_resize_measurement_with_viewport_changes(id, unsafe {
                    (*layout).into()
                })
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `draft` must point to a readable `SnowActiveTextDraftPresentation`.
/// `draft.text_utf8` must either be null with `draft.text_utf8_len == 0`, or point to
/// `draft.text_utf8_len` readable bytes.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_set_active_text_draft_presentation_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    draft: *const SnowActiveTextDraftPresentation,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if draft.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        let draft = match active_text_draft_from_c(unsafe { &*draft }) {
            Ok(draft) => draft,
            Err(error) => return error,
        };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .set_active_text_draft_presentation_with_viewport_changes(id, draft)
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
pub unsafe extern "C" fn snow_viewport_clear_active_text_draft_presentation_ex(
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
                .clear_active_text_draft_presentation_with_viewport_changes(id)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_info`, `out_style`, and `out_active` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_active_text_draft_presentation(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_info: *mut SnowTextElementInfo,
    out_style: *mut SnowTextStyle,
    out_active: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null() || out_style.is_null() || out_active.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let draft = runtime
                    .active_text_draft_presentation(id)
                    .map_err(SnowError::from)?;
                write_out(out_active, u8::from(draft.is_some()));
                if let Some((info, style)) = draft {
                    write_out(out_info, snow_text_element_info_from_rust(info));
                    write_out(out_style, style.into());
                }
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_bound` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_is_text_bound_to_serial_number(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    id: SnowElementId,
    out_bound: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_bound.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, _| {
                let bound = runtime.is_text_bound_to_serial_number(ElementId {
                    index: id.index,
                    generation: id.generation,
                });
                write_out(out_bound, u8::from(bound));
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` is non-null, it must be a live handle returned by this library.
/// `out_info` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_get_text_element(
    runtime: SnowRuntime,
    id: SnowElementId,
    out_info: *mut SnowTextElementInfo,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_ref(runtime, |runtime| {
            let info = runtime
                .text_element_info(ElementId {
                    index: id.index,
                    generation: id.generation,
                })
                .map_err(SnowError::from)?;
            write_out(out_info, snow_text_element_info_from_rust(info));
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `draft` must point to a readable `SnowTextCommitDraft`.
/// `draft.text_utf8` must either be null with `draft.text_utf8_len == 0`, or point to
/// `draft.text_utf8_len` readable bytes.
/// `out_changed_viewports` must be valid for writes.
///
/// Typed draft commit path: persists the supplied text, exact host layout,
/// auto-resize state, and full text style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_commit_text_draft_payload_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    draft: *const SnowTextCommitDraft,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if draft.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        let draft = match text_draft_commit_from_c(unsafe { &*draft }) {
            Ok(draft) => draft,
            Err(error) => return error,
        };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let result = state
                .runtime
                .commit_text_draft_with_viewport_changes(id, draft)
                .map_err(SnowError::from)?;
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_text_id`, `out_has_text_id`, and `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_create_serial_number_text_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    measured_width: f64,
    measured_height: f64,
    out_text_id: *mut SnowElementId,
    out_has_text_id: *mut u8,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_text_id.is_null() || out_has_text_id.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let (result, text_id) = state
                .runtime
                .create_serial_number_text_with_viewport_changes(
                    id,
                    TextLayoutSize {
                        width: measured_width,
                        height: measured_height,
                    },
                )
                .map_err(SnowError::from)?;
            if let Some(text_id) = text_id {
                write_out(out_text_id, snow_element_id_from_rust(text_id));
                write_out(out_has_text_id, 1);
            } else {
                write_out(out_text_id, SnowElementId::default());
                write_out(out_has_text_id, 0);
            }
            write_changed_viewports(out_changed_viewports, result.changed_viewports);
            Ok(())
        }))
    })
}

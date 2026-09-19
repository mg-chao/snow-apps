use snow_draw_engine::{ElementId, Point, TextLayoutSize};
use snow_draw_engine_document::{
    SerialNumberType, SerialPaintGeometry, TextPaintGeometry, resolve_serial_paint_text_connection,
    text_fill_outset, text_paint_bounds, text_paint_outset,
};

use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::text::{
    active_text_draft_from_c, copy_optional_str_to_c_char_field, text_draft_commit_from_c,
    text_string_from_raw,
};
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
                    TextLayoutSize::new(measured_width, measured_height),
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
                    TextLayoutSize::new(measured_width, measured_height),
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

/// # Safety
/// `runtime` and `viewport` must be live handles created by this library.
/// `out_text_id` and `out_has_text_id` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_take_text_edit_request(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_text_id: *mut SnowElementId,
    out_has_text_id: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_text_id.is_null() || out_has_text_id.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let text_id = state
                .runtime
                .take_text_edit_request(id)
                .map_err(SnowError::from)?;
            write_out(out_has_text_id, u8::from(text_id.is_some()));
            write_out(
                out_text_id,
                text_id.map(snow_element_id_from_rust).unwrap_or_default(),
            );
            Ok(())
        }))
    })
}

/// # Safety
/// `runtime` and `viewport` must be live handles created by this library.
/// `out_requested` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_take_new_text_draft_request(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_requested: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_requested.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let requested = state
                .runtime
                .take_new_text_draft_request(id)
                .map_err(SnowError::from)?;
            write_out(out_requested, u8::from(requested));
            Ok(())
        }))
    })
}
/// if any. The host measures the empty draft for the requested font and applies
/// the result via `snow_viewport_apply_serial_label_layout_ex`.
/// # Safety
/// `runtime` and `viewport` must be live handles created by this library.
/// `out_request` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_serial_label_layout_request(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_request: *mut SnowSerialLabelLayoutRequest,
    out_has_request: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_request.is_null() || out_has_request.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |engine, id| {
                let request = engine
                    .serial_number_label_layout_request(id)
                    .map_err(SnowError::from)?;
                write_out(out_has_request, u8::from(request.is_some()));
                write_out(
                    out_request,
                    request
                        .map(|request| {
                            let mut out = SnowSerialLabelLayoutRequest {
                                text_id: snow_element_id_from_rust(request.text_id),
                                font_size: request.font_size,
                                ..SnowSerialLabelLayoutRequest::default()
                            };
                            copy_optional_str_to_c_char_field(
                                &mut out.font_family_utf8,
                                &mut out.font_family_utf8_len,
                                &mut out.font_family_truncated,
                                request.font_family.as_deref(),
                            );
                            out
                        })
                        .unwrap_or_default(),
                );
                Ok(())
            },
        ))
    })
}

/// Applies a host-measured layout to the drag-attached serial number label.
/// # Safety
/// `runtime` and `viewport` must be live handles created by this library.
/// `out_changed_viewports` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_apply_serial_label_layout_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    text_id: SnowElementId,
    layout: SnowTextLayoutSize,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |engine, id| {
                let result = engine
                    .apply_serial_number_label_layout(
                        id,
                        snow_element_id_to_rust(text_id),
                        layout.into(),
                    )
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed_viewports, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

fn text_paint_geometry_from_c(item: &SnowSceneDisplayItem) -> TextPaintGeometry {
    // Display items carry the resolved content box; a default-constructed
    // item has no measurement, and the single fallback decides that.
    let (content_width, content_height) = TextLayoutSize::with_content(
        item.width,
        item.height,
        item.content_width,
        item.content_height,
    )
    .ink_or_wrap();
    TextPaintGeometry {
        center: Point::new(item.center_x, item.center_y),
        width: item.width,
        height: item.height,
        rotation: item.rotation,
        content_width,
        content_height,
        horizontal_align: snow_text_horizontal_align_to_rust(item.text_horizontal_align),
        vertical_align: snow_text_vertical_align_to_rust(item.text_vertical_align),
        has_text: item.text_utf8_len != 0,
        font_size: item.font_size,
        fill: item.fill.into(),
        stroke: item.stroke.into(),
        stroke_width: item.stroke_width,
    }
}

fn serial_number_type_from_c(value: u8) -> Option<SerialNumberType> {
    Some(snow_serial_number_type_to_rust(
        <SnowSerialNumberType as crate::abi::raw_enum::SnowRawEnum>::from_raw(i32::from(value))?,
    ))
}

fn serial_paint_geometry_from_c(item: &SnowSceneDisplayItem) -> Option<SerialPaintGeometry> {
    Some(SerialPaintGeometry {
        center: Point::new(item.center_x, item.center_y),
        diameter: item.width.min(item.height),
        rotation: item.rotation,
        serial_number_type: serial_number_type_from_c(item.serial_number_type)?,
        stroke_width: item.stroke_width,
        corner_radius: item.corner_radii.top_left,
    })
}

/// Painted text outset for host dirty regions; combines the fill padding with
/// the text stroke halo. The text painter measures its background pill padding
/// through `snow_scene_text_fill_outset` instead.
/// # Safety
/// `item` may be null; non-null pointers must reference a readable display item.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_scene_text_paint_outset(
    item: *const SnowSceneDisplayItem,
) -> SnowTextPaintOutset {
    ffi_value(SnowTextPaintOutset::default(), || {
        let Some(item) = (unsafe { item.as_ref() }) else {
            return SnowTextPaintOutset::default();
        };
        if item.kind != SnowSceneDisplayItemKind::Text {
            return SnowTextPaintOutset::default();
        }
        let (x, y) = text_paint_outset(&text_paint_geometry_from_c(item));
        SnowTextPaintOutset { x, y }
    })
}

/// Fill-pill padding the text painter must draw around every line. The host
/// painter consumes this instead of measuring its own padding, so the painted
/// pill and the dirty regions cannot drift apart.
/// # Safety
/// `item` may be null; non-null pointers must reference a readable display item.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_scene_text_fill_outset(
    item: *const SnowSceneDisplayItem,
) -> SnowTextPaintOutset {
    ffi_value(SnowTextPaintOutset::default(), || {
        let Some(item) = (unsafe { item.as_ref() }) else {
            return SnowTextPaintOutset::default();
        };
        if item.kind != SnowSceneDisplayItemKind::Text {
            return SnowTextPaintOutset::default();
        }
        let (x, y) = text_fill_outset(&text_paint_geometry_from_c(item));
        SnowTextPaintOutset { x, y }
    })
}

/// Conservative document-space ink bounds of a text item: the aligned content
/// box expanded by the fill padding and the text stroke halo. Host dirty
/// regions and visibility culling consume this instead of reconstructing the
/// arithmetic, so they cannot drift from the engine's bounds.
/// # Safety
/// `item` may be null; non-null pointers must reference a readable display item.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_scene_text_paint_bounds(
    item: *const SnowSceneDisplayItem,
) -> SnowTextPaintBounds {
    ffi_value(SnowTextPaintBounds::default(), || {
        let Some(item) = (unsafe { item.as_ref() }) else {
            return SnowTextPaintBounds::default();
        };
        if item.kind != SnowSceneDisplayItemKind::Text {
            return SnowTextPaintBounds::default();
        }
        let bounds = text_paint_bounds(&text_paint_geometry_from_c(item));
        SnowTextPaintBounds {
            min_x: bounds.min_x,
            min_y: bounds.min_y,
            max_x: bounds.max_x,
            max_y: bounds.max_y,
        }
    })
}

/// Resolve a serial-number connector from the displayed serial and text items.
/// # Safety
/// Pointers may be null. Non-null `serial` and `text` must be readable;
/// `out_connection` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_scene_resolve_serial_text_connection(
    serial: *const SnowSceneDisplayItem,
    text: *const SnowSceneDisplayItem,
    out_connection: *mut SnowSerialTextConnection,
) -> u8 {
    ffi_value(0, || {
        if out_connection.is_null() {
            return 0;
        }
        let Some(serial) = (unsafe { serial.as_ref() }) else {
            return 0;
        };
        let Some(text) = (unsafe { text.as_ref() }) else {
            return 0;
        };
        if serial.kind != SnowSceneDisplayItemKind::SerialNumber
            || text.kind != SnowSceneDisplayItemKind::Text
        {
            return 0;
        }
        let Some(serial_geometry) = serial_paint_geometry_from_c(serial) else {
            return 0;
        };
        let Some(connection) = resolve_serial_paint_text_connection(
            &serial_geometry,
            &text_paint_geometry_from_c(text),
        ) else {
            return 0;
        };
        let has_baseline =
            connection.text_baseline_start.is_some() && connection.text_baseline_end.is_some();
        let baseline_start = connection.text_baseline_start.unwrap_or_default();
        let baseline_end = connection.text_baseline_end.unwrap_or_default();
        write_out(
            out_connection,
            SnowSerialTextConnection {
                start_x: connection.start.x,
                start_y: connection.start.y,
                end_x: connection.end.x,
                end_y: connection.end.y,
                baseline_start_x: baseline_start.x,
                baseline_start_y: baseline_start.y,
                baseline_end_x: baseline_end.x,
                baseline_end_y: baseline_end.y,
                has_baseline: u8::from(has_baseline),
                reserved: [0; 7],
            },
        );
        1
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn zero_item() -> SnowSceneDisplayItem {
        unsafe { std::mem::zeroed() }
    }

    #[test]
    fn text_paint_outset_ffi_matches_filled_padding_contract() {
        let mut item = zero_item();
        item.kind = SnowSceneDisplayItemKind::Text;
        item.width = 80.0;
        item.height = 40.0;
        item.font_size = 40.0;
        item.fill = SnowColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        item.text_utf8_len = 4;
        let outset = unsafe { snow_scene_text_paint_outset(&item) };
        let line_height = 40.0_f64.max(1.0) * 1.2;
        assert!((outset.x - line_height * 0.32).abs() < 1e-9);
        assert!((outset.y - line_height * 0.1).abs() < 1e-9);
    }

    #[test]
    fn text_fill_outset_ffi_is_the_painter_padding_contract() {
        let mut item = zero_item();
        item.kind = SnowSceneDisplayItemKind::Text;
        item.width = 80.0;
        item.height = 40.0;
        item.font_size = 40.0;
        item.fill = SnowColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        item.text_utf8_len = 4;
        let fill = unsafe { snow_scene_text_fill_outset(&item) };
        let line_height = 40.0_f64.max(1.0) * 1.2;
        assert!((fill.x - line_height * 0.32).abs() < 1e-9);
        assert!((fill.y - line_height * 0.1).abs() < 1e-9);

        // A dominant stroke widens the paint (dirty-region) outset but must not
        // move the painted pill edge.
        item.stroke = SnowColorRgba8 {
            r: 0,
            g: 0,
            b: 0,
            a: 0xff,
        };
        item.stroke_width = 40.0;
        let paint = unsafe { snow_scene_text_paint_outset(&item) };
        let fill_with_stroke = unsafe { snow_scene_text_fill_outset(&item) };
        assert_eq!(paint.x, 20.0);
        assert_eq!(paint.y, 20.0);
        assert_eq!(fill_with_stroke, fill);
    }

    #[test]
    fn serial_text_connection_ffi_ignores_text_fill() {
        let mut text = zero_item();
        text.kind = SnowSceneDisplayItemKind::Text;
        text.center_x = 130.0;
        text.center_y = 10.0;
        text.width = 80.0;
        text.height = 40.0;
        text.font_size = 40.0;
        text.fill = SnowColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        text.text_utf8_len = 4;

        let mut serial = zero_item();
        serial.kind = SnowSceneDisplayItemKind::SerialNumber;
        serial.width = 24.0;
        serial.height = 24.0;
        serial.stroke_width = 2.0;

        let mut connection = SnowSerialTextConnection::default();
        assert_eq!(
            unsafe { snow_scene_resolve_serial_text_connection(&serial, &text, &mut connection) },
            1
        );
        assert_ne!(connection.has_baseline, 0);
        // The underline sits on the aligned ink box; the background fill
        // padding moves the painted pill, never the connector.
        assert!((connection.baseline_start_x - (130.0 - 40.0)).abs() < 1e-9);
        assert!((connection.baseline_end_x - (130.0 + 40.0)).abs() < 1e-9);
        assert!((connection.baseline_end_y - (10.0 + 20.0)).abs() < 1e-9);
    }

    #[test]
    fn serial_text_connection_ffi_honors_aligned_wrapped_ink() {
        let mut text = zero_item();
        text.kind = SnowSceneDisplayItemKind::Text;
        text.center_x = 130.0;
        text.center_y = 10.0;
        text.width = 80.0;
        text.height = 40.0;
        // Wrapped ink narrower than the wrap rectangle, top-left aligned.
        text.content_width = 40.0;
        text.content_height = 20.0;
        text.text_horizontal_align = SnowTextHorizontalAlign::Left;
        text.text_vertical_align = SnowTextVerticalAlign::Top;
        text.font_size = 40.0;
        text.fill = SnowColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        text.text_utf8_len = 4;

        let mut serial = zero_item();
        serial.kind = SnowSceneDisplayItemKind::SerialNumber;
        serial.width = 24.0;
        serial.height = 24.0;
        serial.stroke_width = 2.0;

        let mut connection = SnowSerialTextConnection::default();
        assert_eq!(
            unsafe { snow_scene_resolve_serial_text_connection(&serial, &text, &mut connection) },
            1
        );
        assert_ne!(connection.has_baseline, 0);
        // The underline spans the aligned ink block: left edge at the item's
        // left, bottom at the item top plus the ink height.
        assert!((connection.baseline_start_x - (130.0 - 40.0)).abs() < 1e-9);
        assert!((connection.baseline_end_x - (130.0 - 40.0 + 40.0)).abs() < 1e-9);
        assert!((connection.baseline_end_y - (10.0 - 20.0 + 20.0)).abs() < 1e-9);
    }

    #[test]
    fn text_paint_bounds_ffi_tracks_aligned_ink() {
        let mut item = zero_item();
        item.kind = SnowSceneDisplayItemKind::Text;
        item.center_x = 100.0;
        item.center_y = 100.0;
        item.width = 80.0;
        item.height = 40.0;
        item.content_width = 40.0;
        item.content_height = 20.0;
        item.text_horizontal_align = SnowTextHorizontalAlign::Right;
        item.text_vertical_align = SnowTextVerticalAlign::Bottom;
        item.font_size = 40.0;
        item.fill = SnowColorRgba8 {
            r: 0xff,
            g: 0xff,
            b: 0xff,
            a: 0xff,
        };
        item.text_utf8_len = 4;

        let bounds = unsafe { snow_scene_text_paint_bounds(&item) };
        let line_height = 40.0_f64.max(1.0) * 1.2;
        let pad_x = line_height * 0.32;
        let pad_y = line_height * 0.1;
        // Right/bottom-aligned ink hugs the item's bottom-right corner.
        assert!((bounds.max_x - (100.0 + 40.0 + pad_x)).abs() < 1e-9);
        assert!((bounds.max_y - (100.0 + 20.0 + pad_y)).abs() < 1e-9);
        assert!((bounds.min_x - (100.0 + 40.0 - 40.0 - pad_x)).abs() < 1e-9);
        assert!((bounds.min_y - (100.0 + 20.0 - 20.0 - pad_y)).abs() < 1e-9);
    }
}

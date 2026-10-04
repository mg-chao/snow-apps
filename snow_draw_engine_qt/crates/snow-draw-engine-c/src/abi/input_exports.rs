use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::types::*;

/// Returns the topmost element eligible for body selection with the given button.
/// Coordinates are in canvas space; tolerance comes from the queried viewport.
///
/// # Safety
/// Handles must be live and output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_hit_quick_selection(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    canvas_x: f64,
    canvas_y: f64,
    button: SnowPointerButton,
    out_id: *mut SnowElementId,
    out_hit: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_id.is_null() || out_hit.is_null() || !canvas_x.is_finite() || !canvas_y.is_finite() {
            return SnowError::InvalidArgument;
        }
        let button = match button {
            SnowPointerButton::Primary => snow_draw_engine::PointerButton::Primary,
            SnowPointerButton::Secondary => snow_draw_engine::PointerButton::Secondary,
            _ => return SnowError::InvalidArgument,
        };
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |runtime, id| {
                let hit = runtime
                    .hit_quick_selection_at(
                        id,
                        snow_draw_engine_core::Point::new(canvas_x, canvas_y),
                        button,
                    )
                    .map_err(SnowError::from)?;
                write_out(
                    out_id,
                    hit.map_or(SnowElementId::default(), snow_element_id_from_rust),
                );
                write_out(out_hit, u8::from(hit.is_some()));
                Ok(())
            },
        ))
    })
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `event` must point to a readable `SnowInputEvent`.
/// `out_output` and `out_changed_viewports` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_process_input_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    event: *const SnowInputEvent,
    out_output: *mut SnowInteractionOutput,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if event.is_null() || out_output.is_null() || out_changed_viewports.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let event = snow_input_event_to_rust(unsafe { &*event })?;
            let update = state
                .runtime
                .process_input_with_viewport_changes(id, event)
                .map_err(SnowError::from)?;
            write_out(
                out_output,
                snow_interaction_output_from_rust(update.interaction),
            );
            write_changed_viewports(out_changed_viewports, update.changed_viewports);
            Ok(())
        }))
    })
}

/// Processes an ordered, non-empty batch containing only pointer-move events.
/// Viewport presentation is refreshed once after the final event.
///
/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `events` must point to `event_count` readable values. Output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_process_pointer_move_batch_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    events: *const SnowInputEvent,
    event_count: u32,
    out_output: *mut SnowInteractionOutput,
    out_changed_viewports: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if events.is_null()
            || event_count == 0
            || out_output.is_null()
            || out_changed_viewports.is_null()
        {
            return SnowError::InvalidArgument;
        }
        let events = match unsafe { std::slice::from_raw_parts(events, event_count as usize) }
            .iter()
            .map(snow_input_event_to_rust)
            .collect::<Result<Vec<_>, _>>()
        {
            Ok(events) => events,
            Err(error) => return error,
        };
        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let update = state
                .runtime
                .process_pointer_move_batch_with_viewport_changes(id, &events)
                .map_err(SnowError::from)?;
            write_out(
                out_output,
                snow_interaction_output_from_rust(update.interaction),
            );
            write_changed_viewports(out_changed_viewports, update.changed_viewports);
            Ok(())
        }))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine::{
        ActiveTool, InputEvent, Modifiers, Point, PointerButton, PointerButtons, PointerDevice,
        PointerEvent, PointerEventType, Runtime, ViewportConfig,
    };

    #[test]
    fn right_quick_selection_query_reports_hits_misses_and_invalid_arguments() {
        let mut state = SnowRuntimeImpl {
            runtime: Runtime::default(),
        };
        let id = state
            .runtime
            .create_viewport(ViewportConfig::default())
            .unwrap();
        state
            .runtime
            .set_viewport_surface_size(id, 200, 200)
            .unwrap();
        state
            .runtime
            .set_viewport_active_tool(id, ActiveTool::Shape)
            .unwrap();
        for (kind, x, y, button) in [
            (PointerEventType::Down, 80.0, 80.0, PointerButton::Primary),
            (PointerEventType::Move, 120.0, 120.0, PointerButton::Primary),
            (PointerEventType::Up, 120.0, 120.0, PointerButton::Primary),
            (
                PointerEventType::Down,
                100.0,
                80.0,
                PointerButton::Secondary,
            ),
            (PointerEventType::Up, 100.0, 80.0, PointerButton::Secondary),
        ] {
            state
                .runtime
                .process_input(
                    id,
                    InputEvent::Pointer(PointerEvent {
                        pointer_id: 1,
                        event_type: kind,
                        device: PointerDevice::Mouse,
                        position: Point::new(x, y),
                        button: Some(button),
                        buttons: PointerButtons(if kind == PointerEventType::Up {
                            0
                        } else if button == PointerButton::Primary {
                            PointerButtons::PRIMARY
                        } else {
                            PointerButtons::SECONDARY
                        }),
                        modifiers: Modifiers::default(),
                    }),
                )
                .unwrap();
        }
        state
            .runtime
            .set_quick_selection_disabled_tools(u64::MAX)
            .unwrap();
        let selected = state.runtime.selected_ids();
        let mut viewport = SnowViewportImpl { id };
        let mut out_id = SnowElementId::default();
        let mut hit = 0;
        unsafe {
            assert_eq!(
                snow_viewport_hit_quick_selection(
                    &mut state,
                    &mut viewport,
                    0.0,
                    -20.0,
                    SnowPointerButton::Secondary,
                    &mut out_id,
                    &mut hit
                ),
                SnowError::Ok
            );
            assert_eq!(hit, 1);
            assert_eq!(out_id, snow_element_id_from_rust(selected[0]));
            assert_eq!(
                snow_viewport_hit_quick_selection(
                    &mut state,
                    &mut viewport,
                    0.0,
                    -20.0,
                    SnowPointerButton::Primary,
                    &mut out_id,
                    &mut hit
                ),
                SnowError::Ok
            );
            assert_eq!(hit, 0);
            assert_eq!(out_id, SnowElementId::default());
            assert_eq!(
                snow_viewport_hit_quick_selection(
                    &mut state,
                    &mut viewport,
                    f64::NAN,
                    0.0,
                    SnowPointerButton::Secondary,
                    &mut out_id,
                    &mut hit
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_viewport_hit_quick_selection(
                    &mut state,
                    &mut viewport,
                    0.0,
                    0.0,
                    SnowPointerButton::Middle,
                    &mut out_id,
                    &mut hit
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_viewport_hit_quick_selection(
                    &mut state,
                    &mut viewport,
                    0.0,
                    0.0,
                    SnowPointerButton::Secondary,
                    &mut out_id,
                    std::ptr::null_mut()
                ),
                SnowError::InvalidArgument
            );
        }
        assert_eq!(state.runtime.selected_ids(), selected);
    }
}

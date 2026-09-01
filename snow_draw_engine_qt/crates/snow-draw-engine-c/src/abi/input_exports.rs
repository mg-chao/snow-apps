use crate::abi::convert::*;
use crate::abi::handles::*;
use crate::abi::types::*;

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

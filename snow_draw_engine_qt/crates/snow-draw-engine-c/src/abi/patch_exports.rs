use crate::abi::convert::snow_patch_cursor_to_rust;
use crate::abi::handles::*;
use crate::abi::patch::SnowPatchPayload;
use crate::abi::types::*;

#[unsafe(no_mangle)]
pub extern "C" fn snow_filter_render_spec_resolve(
    filter_type: u32,
    strength: f64,
) -> SnowFilterRenderSpec {
    let filter_type = match filter_type {
        1 => snow_draw_engine::DisplayFilterType::GaussianBlur,
        2 => snow_draw_engine::DisplayFilterType::Grayscale,
        3 => snow_draw_engine::DisplayFilterType::Inversion,
        4 => snow_draw_engine::DisplayFilterType::Emboss,
        5 => snow_draw_engine::DisplayFilterType::SmartErase,
        _ => snow_draw_engine::DisplayFilterType::Mosaic,
    };
    let spec = snow_draw_engine::FilterRenderSpec::resolve(filter_type, strength);
    SnowFilterRenderSpec {
        filter_type: match spec.filter_type {
            snow_draw_engine::DisplayFilterType::Mosaic => 0,
            snow_draw_engine::DisplayFilterType::GaussianBlur => 1,
            snow_draw_engine::DisplayFilterType::Grayscale => 2,
            snow_draw_engine::DisplayFilterType::Inversion => 3,
            snow_draw_engine::DisplayFilterType::Emboss => 4,
            snow_draw_engine::DisplayFilterType::SmartErase => 5,
        },
        render_phase: 0,
        strength: spec.strength,
        mosaic_block_size: spec.mosaic_block_size,
        blur_sigma: spec.blur_sigma,
        sampling_radius: spec.sampling_radius,
    }
}

/// # Safety
/// If `runtime` and `viewport` are non-null, they must be live handles created by this library.
/// `out_patch` must be valid for writes of one `SnowPatchHandle` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_acquire_patch(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    cursor: *const SnowPatchCursor,
    out_patch: *mut SnowPatchHandle,
) -> SnowError {
    ffi_error(|| {
        if out_patch.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_runtime_impl_mut(runtime, |state| {
            let id = viewport_id(viewport)?;
            let cursor = (!cursor.is_null()).then(|| unsafe { &*cursor }).copied();
            let patch = state
                .runtime
                .acquire_patch(id, cursor.map(snow_patch_cursor_to_rust))
                .map_err(SnowError::from)?;
            let payload = SnowPatchPayload::from_patch(&patch);
            write_out(
                out_patch,
                Box::into_raw(Box::new(SnowPatchHandleImpl { payload })),
            );
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// The handle must not be used again after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_destroy(patch: SnowPatchHandle) {
    ffi_void(|| {
        if patch.is_null() {
            return;
        }
        unsafe {
            drop(Box::from_raw(patch));
        }
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_info` must be valid for writes of one `SnowPatchInfo` value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_info(
    patch: SnowPatchHandle,
    out_info: *mut SnowPatchInfo,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_out(out_info, patch.payload.info);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_ops` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_scene_ops(
    patch: SnowPatchHandle,
    out_ops: *mut *const SnowPatchOp,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ops.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ops, out_count, &patch.payload.scene_ops);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_ops` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_overlay_ops(
    patch: SnowPatchHandle,
    out_ops: *mut *const SnowPatchOp,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ops.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ops, out_count, &patch.payload.overlay_ops);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live patch handle. Output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_pen_filter_geometry_ops(
    patch: SnowPatchHandle,
    out_ops: *mut *const SnowPenFilterGeometryPatch,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ops.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ops, out_count, &patch.payload.pen_filter_geometry_ops);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live patch handle. Output pointers must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_pen_filter_geometry_points(
    patch: SnowPatchHandle,
    out_points: *mut *const SnowArrowPoint,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_points.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(
                out_points,
                out_count,
                &patch.payload.pen_filter_geometry_points,
            );
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_path_geometry_ops(
    patch: SnowPatchHandle,
    out_ops: *mut *const SnowPathGeometryPatch,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ops.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ops, out_count, &patch.payload.path_geometry_ops);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_path_geometry_ranges(
    patch: SnowPatchHandle,
    out_ranges: *mut *const SnowPathChunkRange,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ranges.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ranges, out_count, &patch.payload.path_geometry_ranges);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_path_geometry_chunks(
    patch: SnowPatchHandle,
    out_chunks: *mut *const SnowPathChunk,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_chunks.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_chunks, out_count, &patch.payload.path_geometry_chunks);
            Ok(())
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_path_geometry_commands(
    patch: SnowPatchHandle,
    out_commands: *mut *const SnowArrowPathCommand,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_commands.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(
                out_commands,
                out_count,
                &patch.payload.path_geometry_commands,
            );
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_ops` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_spotlight_ops(
    patch: SnowPatchHandle,
    out_ops: *mut *const SnowPatchOp,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_ops.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_ops, out_count, &patch.payload.spotlight_ops);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_items` and `out_count` must be valid for writes.
/// Returned items and their nested pointer fields remain valid until `patch` is released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_scene_items(
    patch: SnowPatchHandle,
    out_items: *mut *const SnowSceneDisplayItem,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_items.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_items, out_count, &patch.payload.scene_items);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_items` and `out_count` must be valid for writes.
/// Returned items and their nested pointer fields remain valid until `patch` is released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_overlay_items(
    patch: SnowPatchHandle,
    out_items: *mut *const SnowOverlayDisplayItem,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_items.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_items, out_count, &patch.payload.overlay_items);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_items` and `out_count` must be valid for writes. Returned storage borrows from `patch`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_spotlight_cutouts(
    patch: SnowPatchHandle,
    out_items: *mut *const SnowSpotlightCutout,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_items.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_items, out_count, &patch.payload.spotlight_cutouts);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_rects` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_scene_dirty_rects(
    patch: SnowPatchHandle,
    out_rects: *mut *const SnowDirtyRect,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_rects.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_rects, out_count, &patch.payload.scene_dirty_rects);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_rects` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_overlay_dirty_rects(
    patch: SnowPatchHandle,
    out_rects: *mut *const SnowDirtyRect,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_rects.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_rects, out_count, &patch.payload.overlay_dirty_rects);
            Ok(())
        }))
    })
}

/// # Safety
/// If `patch` is non-null, it must be a live handle returned by `snow_viewport_acquire_patch`.
/// `out_rects` and `out_count` must be valid for writes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_decoration_dirty_rects(
    patch: SnowPatchHandle,
    out_rects: *mut *const SnowDirtyRect,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_rects.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }

        ffi_status(with_patch_ref(patch, |patch| {
            write_slice_out(out_rects, out_count, &patch.payload.decoration_dirty_rects);
            Ok(())
        }))
    })
}

#[cfg(test)]
mod filter_render_spec_export_tests {
    use super::*;

    #[test]
    fn emboss_resolver_preserves_abi_layout_and_normalized_strength() {
        assert_eq!(std::mem::size_of::<SnowFilterRenderSpec>(), 40);
        let spec = snow_filter_render_spec_resolve(4, 0.5);
        assert_eq!(spec.filter_type, 4);
        assert_eq!(spec.render_phase, 0);
        assert_eq!(spec.strength, 0.5);
        assert_eq!(spec.sampling_radius, 1.0);
    }
}

#[cfg(test)]
mod spotlight_patch_export_tests {
    use super::*;
    use snow_draw_engine::{DisplaySpotlightCutout, ReplaceRangeOp, ViewportPatch};

    #[test]
    fn spotlight_payload_exports_reject_null_output_pointers() {
        let mut ops = std::ptr::null();
        let mut cutouts = std::ptr::null();
        let mut count = 0;
        unsafe {
            assert_eq!(
                snow_patch_get_spotlight_ops(
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &mut count
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_patch_get_spotlight_ops(std::ptr::null_mut(), &mut ops, std::ptr::null_mut()),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_patch_get_spotlight_cutouts(
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    &mut count,
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_patch_get_spotlight_cutouts(
                    std::ptr::null_mut(),
                    &mut cutouts,
                    std::ptr::null_mut(),
                ),
                SnowError::InvalidArgument
            );
        }
    }

    #[test]
    fn spotlight_payload_storage_borrows_from_the_patch_handle() {
        let mut patch = ViewportPatch::default();
        patch.decoration.spotlight_ops = vec![ReplaceRangeOp {
            start: 3,
            delete_count: 2,
            insert_items: vec![DisplaySpotlightCutout {
                center_x: 11.0,
                center_y: 12.0,
                width: 30.0,
                height: 40.0,
                rotation: 0.25,
            }],
        }];
        let payload = SnowPatchPayload::from_patch(&patch);
        let handle = Box::into_raw(Box::new(SnowPatchHandleImpl { payload }));
        let mut ops = std::ptr::null();
        let mut cutouts = std::ptr::null();
        let mut op_count = 0;
        let mut cutout_count = 0;

        unsafe {
            assert_eq!(
                snow_patch_get_spotlight_ops(handle, &mut ops, &mut op_count),
                SnowError::Ok
            );
            assert_eq!(
                snow_patch_get_spotlight_cutouts(handle, &mut cutouts, &mut cutout_count),
                SnowError::Ok
            );
            assert_eq!(op_count, 1);
            assert_eq!(cutout_count, 1);
            assert!(!ops.is_null());
            assert!(!cutouts.is_null());
            assert_eq!((*ops).start, 3);
            assert_eq!((*ops).delete_count, 2);
            assert_eq!((*cutouts).center_x, 11.0);
            assert_eq!((*cutouts).rotation, 0.25);
            snow_patch_destroy(handle);
        }
    }
}

/// # Safety
/// `patch` must be a live patch handle. Output pointers must be valid for writes.
/// The returned array remains valid until the patch is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_patch_get_scene_render_plan(
    patch: SnowPatchHandle,
    out_replace: *mut u8,
    out_runs: *mut *const SnowSceneRenderRun,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_replace.is_null() || out_runs.is_null() || out_count.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_patch_ref(patch, |patch| {
            write_out(out_replace, u8::from(patch.payload.render_plan_replace));
            write_slice_out(out_runs, out_count, &patch.payload.render_plan);
            Ok(())
        }))
    })
}

#[cfg(test)]
mod render_plan_tests {
    use super::*;
    use snow_draw_engine::{DisplayItemId, SceneRenderRun, ViewportPatch};

    #[test]
    fn render_plan_replacement_roundtrips_and_borrows_patch_storage() {
        let id = DisplayItemId {
            index: 71,
            generation: 9,
        };
        for metadata in [
            None,
            Some(Vec::new()),
            Some(vec![SceneRenderRun {
                source_pass: id,
                effect_run: DisplayItemId {
                    index: 82,
                    generation: 11,
                },
                start: 4,
                count: 2,
            }]),
        ] {
            let patch = ViewportPatch {
                scene_render_plan: metadata.clone(),
                ..Default::default()
            };
            let handle = Box::into_raw(Box::new(SnowPatchHandleImpl {
                payload: SnowPatchPayload::from_patch(&patch),
            }));
            let mut replace = 99;
            let mut runs = std::ptr::null();
            let mut count = 99;
            unsafe {
                assert_eq!(
                    snow_patch_get_scene_render_plan(handle, &mut replace, &mut runs, &mut count),
                    SnowError::Ok
                );
                assert_eq!(replace, u8::from(metadata.is_some()));
                assert_eq!(count as usize, metadata.as_ref().map_or(0, Vec::len));
                if count != 0 {
                    assert_eq!((*runs).source_pass.index, 71);
                    assert_eq!((*runs).source_pass.generation, 9);
                    assert_eq!((*runs).effect_run.generation, 11);
                    assert_eq!((*runs).start, 4);
                    assert_eq!((*runs).count, 2);
                    let first = runs;
                    assert_eq!(
                        snow_patch_get_scene_render_plan(
                            handle,
                            &mut replace,
                            &mut runs,
                            &mut count
                        ),
                        SnowError::Ok
                    );
                    assert_eq!(first, runs);
                }
                for (replacement, descriptors, length) in [
                    (
                        std::ptr::null_mut(),
                        &mut runs as *mut _,
                        &mut count as *mut _,
                    ),
                    (
                        &mut replace as *mut _,
                        std::ptr::null_mut(),
                        &mut count as *mut _,
                    ),
                    (
                        &mut replace as *mut _,
                        &mut runs as *mut _,
                        std::ptr::null_mut(),
                    ),
                ] {
                    assert_eq!(
                        snow_patch_get_scene_render_plan(handle, replacement, descriptors, length),
                        SnowError::InvalidArgument
                    );
                }
                snow_patch_destroy(handle);
            }
        }
        assert_eq!(std::mem::size_of::<SnowSceneRenderRun>(), 24);
    }
}

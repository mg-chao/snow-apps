use anyhow::Context;
use std::cell::{Cell, RefCell};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_CPU_ACCESS_READ, D3D11_MAP_READ, D3D11_MAPPED_SUBRESOURCE, D3D11_TEXTURE2D_DESC,
    D3D11_USAGE_STAGING, ID3D11Device, ID3D11DeviceContext, ID3D11Resource, ID3D11Texture2D,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::DXGI_ERROR_WAS_STILL_DRAWING;
use windows::core::Interface;

use crate::backend::CaptureBlitRegion;
use crate::convert::{self, SurfaceConversionOptions, SurfaceLayout, SurfacePixelFormat};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{ColorSpace, DirtyRect, Frame};

pub(crate) enum StagingSampleDesc {
    SingleSample,
}

fn mark_frame_srgb(frame: &mut Frame) {
    frame.metadata.color_space = ColorSpace::Srgb;
}

#[inline]
fn force_rgba_alpha_opaque(bytes: &mut [u8]) {
    debug_assert_eq!(bytes.len() % 4, 0);

    let (prefix, pixels, suffix) = unsafe { bytes.align_to_mut::<u32>() };
    if prefix.is_empty() && suffix.is_empty() {
        let alpha_mask = u32::from_ne_bytes([0, 0, 0, u8::MAX]);
        for pixel in pixels {
            *pixel |= alpha_mask;
        }
        return;
    }

    for alpha in bytes[3..].iter_mut().step_by(4) {
        *alpha = u8::MAX;
    }
}

#[inline]
fn force_frame_alpha_opaque(frame: &mut Frame) {
    force_rgba_alpha_opaque(frame.as_mut_rgba_bytes());
}

fn force_frame_alpha_opaque_in_rects(
    frame: &mut Frame,
    dirty_rects: &[DirtyRect],
    dst_origin_x: u32,
    dst_origin_y: u32,
) -> CaptureResult<()> {
    if dirty_rects.is_empty() {
        return Ok(());
    }

    let frame_width = usize::try_from(frame.width()).map_err(|_| CaptureError::BufferOverflow)?;
    let frame_height = usize::try_from(frame.height()).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_origin_x = usize::try_from(dst_origin_x).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_origin_y = usize::try_from(dst_origin_y).map_err(|_| CaptureError::BufferOverflow)?;
    let row_pitch = frame_width
        .checked_mul(4)
        .ok_or(CaptureError::BufferOverflow)?;
    let bytes = frame.as_mut_rgba_bytes();

    for rect in dirty_rects {
        let rect_x = usize::try_from(rect.x).map_err(|_| CaptureError::BufferOverflow)?;
        let rect_y = usize::try_from(rect.y).map_err(|_| CaptureError::BufferOverflow)?;
        let rect_w = usize::try_from(rect.width).map_err(|_| CaptureError::BufferOverflow)?;
        let rect_h = usize::try_from(rect.height).map_err(|_| CaptureError::BufferOverflow)?;
        let dst_x = dst_origin_x
            .checked_add(rect_x)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_y = dst_origin_y
            .checked_add(rect_y)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_right = dst_x
            .checked_add(rect_w)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_bottom = dst_y
            .checked_add(rect_h)
            .ok_or(CaptureError::BufferOverflow)?;
        if dst_right > frame_width || dst_bottom > frame_height {
            return Err(CaptureError::BufferOverflow);
        }

        for row in 0..rect_h {
            let row_start = dst_y
                .checked_add(row)
                .and_then(|y| y.checked_mul(row_pitch))
                .and_then(|base| dst_x.checked_mul(4).and_then(|xoff| base.checked_add(xoff)))
                .ok_or(CaptureError::BufferOverflow)?;
            let row_bytes = rect_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
            let row_end = row_start
                .checked_add(row_bytes)
                .ok_or(CaptureError::BufferOverflow)?;
            force_rgba_alpha_opaque(&mut bytes[row_start..row_end]);
        }
    }

    Ok(())
}

fn source_surface_format(format: DXGI_FORMAT) -> Option<SurfacePixelFormat> {
    match format {
        DXGI_FORMAT_B8G8R8A8_UNORM | DXGI_FORMAT_B8G8R8A8_UNORM_SRGB => {
            Some(SurfacePixelFormat::Bgra8)
        }
        DXGI_FORMAT_R8G8B8A8_UNORM => Some(SurfacePixelFormat::Rgba8),
        DXGI_FORMAT_R16G16B16A16_FLOAT => Some(SurfacePixelFormat::Rgba16Float),
        _ => None,
    }
}

fn source_row_bytes(format: SurfacePixelFormat, pixel_count: usize) -> CaptureResult<usize> {
    let bytes_per_pixel = source_bytes_per_pixel(format);
    pixel_count
        .checked_mul(bytes_per_pixel)
        .ok_or(CaptureError::BufferOverflow)
}

fn source_bytes_per_pixel(format: SurfacePixelFormat) -> usize {
    match format {
        SurfacePixelFormat::Bgra8 | SurfacePixelFormat::Rgba8 => 4usize,
        SurfacePixelFormat::Rgba16Float => 8usize,
    }
}

const DIRTY_RECT_PARALLEL_MIN_PIXELS: usize = 131_072;
const DIRTY_RECT_PARALLEL_MIN_CHUNK_PIXELS: usize = 32_768;
const DIRTY_RECT_PARALLEL_MAX_WORKERS: usize = usize::MAX;
const DIRTY_RECT_PARALLEL_MIN_RECTS: usize = 4;
const DIRTY_RECT_BATCH_FORCE_TEMPORAL_PIXELS: usize = 0;
const D3D11_MAP_FLAG_DO_NOT_WAIT: u32 = 0x100000;
const D3D11_MAP_SPIN_POLLS_DEFAULT: usize = 6;
const D3D11_MAP_SPIN_POLLS_MIN: usize = 1;
const D3D11_MAP_SPIN_POLLS_MAX: usize = 64;

thread_local! {
    static MAP_SPIN_POLLS_ADAPTIVE: Cell<usize> = const { Cell::new(D3D11_MAP_SPIN_POLLS_DEFAULT) };
    static DIRTY_WORK_ITEMS_SCRATCH: RefCell<Vec<DirtyRectWorkItem>> = const { RefCell::new(Vec::new()) };
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct DirtyRectConversionHints {
    /// Caller guarantees dirty rectangles are already in-bounds for both
    /// source and destination surfaces.
    pub trusted_bounds: bool,
    /// Optional non-empty rectangle count precomputed by the caller.
    pub non_empty_rects: Option<usize>,
    /// Optional total dirty pixel count precomputed by the caller.
    pub total_dirty_pixels: Option<usize>,
}

#[inline(always)]
fn adaptive_map_spin_polls(max_polls: usize) -> usize {
    MAP_SPIN_POLLS_ADAPTIVE.with(|state| {
        let current = state.get().clamp(
            D3D11_MAP_SPIN_POLLS_MIN,
            max_polls.max(D3D11_MAP_SPIN_POLLS_MIN),
        );
        if current != state.get() {
            state.set(current);
        }
        current
    })
}

#[inline(always)]
fn update_adaptive_map_spin_polls(max_polls: usize, success_attempt: Option<usize>) {
    MAP_SPIN_POLLS_ADAPTIVE.with(|state| {
        let current = state.get().clamp(
            D3D11_MAP_SPIN_POLLS_MIN,
            max_polls.max(D3D11_MAP_SPIN_POLLS_MIN),
        );
        let next = match success_attempt {
            Some(0) => current.saturating_sub(1).max(D3D11_MAP_SPIN_POLLS_MIN),
            Some(attempt) => attempt.saturating_add(2).clamp(
                D3D11_MAP_SPIN_POLLS_MIN,
                max_polls.max(D3D11_MAP_SPIN_POLLS_MIN),
            ),
            None => current.saturating_sub(2).max(D3D11_MAP_SPIN_POLLS_MIN),
        };
        state.set(next);
    });
}

#[inline(always)]
fn map_resource_read_with_spin(
    context: &ID3D11DeviceContext,
    resource: &ID3D11Resource,
    map_context: &'static str,
) -> CaptureResult<D3D11_MAPPED_SUBRESOURCE> {
    let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();

    let max_polls = map_spin_poll_count();
    let polls = adaptive_map_spin_polls(max_polls);
    let mut exhausted_spin = true;
    for attempt in 0..polls {
        let map_result = unsafe {
            context.Map(
                resource,
                0,
                D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT,
                Some(&mut mapped),
            )
        };
        match map_result {
            Ok(()) => {
                update_adaptive_map_spin_polls(max_polls, Some(attempt));
                return Ok(mapped);
            }
            Err(error) if error.code() == DXGI_ERROR_WAS_STILL_DRAWING => {
                if attempt + 1 < polls {
                    std::hint::spin_loop();
                }
            }
            Err(_) => {
                exhausted_spin = false;
                break;
            }
        }
    }
    if exhausted_spin {
        update_adaptive_map_spin_polls(max_polls, None);
    }

    mapped = D3D11_MAPPED_SUBRESOURCE::default();
    unsafe { context.Map(resource, 0, D3D11_MAP_READ, 0, Some(&mut mapped)) }
        .context(map_context)
        .map_err(CaptureError::platform)?;
    Ok(mapped)
}

#[inline(always)]
fn map_resource_read_blocking(
    context: &ID3D11DeviceContext,
    resource: &ID3D11Resource,
    map_context: &'static str,
) -> CaptureResult<D3D11_MAPPED_SUBRESOURCE> {
    let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
    unsafe { context.Map(resource, 0, D3D11_MAP_READ, 0, Some(&mut mapped)) }
        .context(map_context)
        .map_err(CaptureError::platform)?;
    Ok(mapped)
}

#[derive(Clone, Copy)]
struct DirtyRectWorkItem {
    src_offset: usize,
    dst_offset: usize,
    x: usize,
    y: usize,
    width: usize,
    height: usize,
}

#[inline(always)]
fn work_items_overlap(a: DirtyRectWorkItem, b: DirtyRectWorkItem) -> bool {
    let a_right = a.x.saturating_add(a.width);
    let a_bottom = a.y.saturating_add(a.height);
    let b_right = b.x.saturating_add(b.width);
    let b_bottom = b.y.saturating_add(b.height);
    a.x < b_right && b.x < a_right && a.y < b_bottom && b.y < a_bottom
}

fn work_items_non_overlapping(work_items: &[DirtyRectWorkItem]) -> bool {
    if work_items.len() <= 1 {
        return true;
    }

    for i in 0..work_items.len() {
        for j in (i + 1)..work_items.len() {
            if work_items_overlap(work_items[i], work_items[j]) {
                return false;
            }
        }
    }
    true
}

fn map_spin_poll_count() -> usize {
    D3D11_MAP_SPIN_POLLS_DEFAULT.clamp(D3D11_MAP_SPIN_POLLS_MIN, D3D11_MAP_SPIN_POLLS_MAX)
}

#[inline(always)]
fn build_dirty_rect_work_item(
    rect: &DirtyRect,
    width: usize,
    height: usize,
    dst_origin_x: usize,
    dst_origin_y: usize,
    dst_width: usize,
    dst_height: usize,
    src_pitch: usize,
    src_bytes_per_pixel: usize,
    dst_pitch: usize,
) -> CaptureResult<Option<DirtyRectWorkItem>> {
    let x = usize::try_from(rect.x).map_err(|_| CaptureError::BufferOverflow)?;
    let y = usize::try_from(rect.y).map_err(|_| CaptureError::BufferOverflow)?;
    let rect_w = usize::try_from(rect.width).map_err(|_| CaptureError::BufferOverflow)?;
    let rect_h = usize::try_from(rect.height).map_err(|_| CaptureError::BufferOverflow)?;

    if rect_w == 0 || rect_h == 0 || x >= width || y >= height {
        return Ok(None);
    }

    let end_x = x.saturating_add(rect_w).min(width);
    let end_y = y.saturating_add(rect_h).min(height);
    if end_x <= x || end_y <= y {
        return Ok(None);
    }

    let copy_w = end_x - x;
    let copy_h = end_y - y;

    let src_row_bytes = copy_w
        .checked_mul(src_bytes_per_pixel)
        .ok_or(CaptureError::BufferOverflow)?;
    let src_end_in_row = x
        .checked_mul(src_bytes_per_pixel)
        .and_then(|offset| offset.checked_add(src_row_bytes))
        .ok_or(CaptureError::BufferOverflow)?;
    if src_end_in_row > src_pitch {
        return Err(CaptureError::BufferOverflow);
    }

    let dst_x = dst_origin_x
        .checked_add(x)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_y = dst_origin_y
        .checked_add(y)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_right = dst_x
        .checked_add(copy_w)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_bottom = dst_y
        .checked_add(copy_h)
        .ok_or(CaptureError::BufferOverflow)?;
    if dst_right > dst_width || dst_bottom > dst_height {
        return Err(CaptureError::BufferOverflow);
    }

    let dst_row_bytes = copy_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
    let dst_end_in_row = dst_x
        .checked_mul(4)
        .and_then(|offset| offset.checked_add(dst_row_bytes))
        .ok_or(CaptureError::BufferOverflow)?;
    if dst_end_in_row > dst_pitch {
        return Err(CaptureError::BufferOverflow);
    }

    let src_offset = y
        .checked_mul(src_pitch)
        .and_then(|base| {
            x.checked_mul(src_bytes_per_pixel)
                .and_then(|xoff| base.checked_add(xoff))
        })
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_offset = dst_y
        .checked_mul(dst_pitch)
        .and_then(|base| dst_x.checked_mul(4).and_then(|xoff| base.checked_add(xoff)))
        .ok_or(CaptureError::BufferOverflow)?;

    Ok(Some(DirtyRectWorkItem {
        src_offset,
        dst_offset,
        x,
        y,
        width: copy_w,
        height: copy_h,
    }))
}

#[inline(always)]
fn dirty_rects_fit_bounds(
    dirty_rects: &[DirtyRect],
    src_width: u32,
    src_height: u32,
    dst_origin_x: u32,
    dst_origin_y: u32,
    dst_width: u32,
    dst_height: u32,
) -> bool {
    for rect in dirty_rects {
        if rect.width == 0 || rect.height == 0 {
            continue;
        }

        let Some(src_right) = rect.x.checked_add(rect.width) else {
            return false;
        };
        let Some(src_bottom) = rect.y.checked_add(rect.height) else {
            return false;
        };
        if src_right > src_width || src_bottom > src_height {
            return false;
        }

        let Some(dst_x) = dst_origin_x.checked_add(rect.x) else {
            return false;
        };
        let Some(dst_y) = dst_origin_y.checked_add(rect.y) else {
            return false;
        };
        let Some(dst_right) = dst_x.checked_add(rect.width) else {
            return false;
        };
        let Some(dst_bottom) = dst_y.checked_add(rect.height) else {
            return false;
        };
        if dst_right > dst_width || dst_bottom > dst_height {
            return false;
        }
    }
    true
}

#[inline(always)]
unsafe fn build_dirty_rect_work_item_trusted(
    rect: DirtyRect,
    dst_origin_x: usize,
    dst_origin_y: usize,
    src_pitch: usize,
    src_bytes_per_pixel: usize,
    dst_pitch: usize,
) -> Option<DirtyRectWorkItem> {
    if rect.width == 0 || rect.height == 0 {
        return None;
    }

    let x = rect.x as usize;
    let y = rect.y as usize;
    let copy_w = rect.width as usize;
    let copy_h = rect.height as usize;
    let dst_x = dst_origin_x + x;
    let dst_y = dst_origin_y + y;

    let src_offset = y * src_pitch + x * src_bytes_per_pixel;
    let dst_offset = dst_y * dst_pitch + dst_x * 4;

    Some(DirtyRectWorkItem {
        src_offset,
        dst_offset,
        x,
        y,
        width: copy_w,
        height: copy_h,
    })
}

#[inline(always)]
unsafe fn convert_dirty_rects_trusted_direct_unchecked(
    format: SurfacePixelFormat,
    converter: convert::SurfaceRowConverter,
    src_base: *const u8,
    src_pitch: usize,
    src_bytes_per_pixel: usize,
    dst_base: *mut u8,
    dst_pitch: usize,
    dirty_rects: &[DirtyRect],
    dst_origin_x: usize,
    dst_origin_y: usize,
    hints: DirtyRectConversionHints,
    force_opaque_alpha: bool,
) -> CaptureResult<usize> {
    let consistent_hints = match (hints.non_empty_rects, hints.total_dirty_pixels) {
        (Some(non_empty_rects), Some(total_dirty_pixels))
            if non_empty_rects <= dirty_rects.len()
                && ((non_empty_rects == 0 && total_dirty_pixels == 0)
                    || (non_empty_rects > 0 && total_dirty_pixels > 0)) =>
        {
            Some((non_empty_rects, total_dirty_pixels))
        }
        _ => None,
    };

    let (converted, total_dirty_pixels) = if let Some(pair) = consistent_hints {
        pair
    } else {
        let mut converted = 0usize;
        let mut total_dirty_pixels = 0usize;
        for rect in dirty_rects {
            let width = rect.width as usize;
            let height = rect.height as usize;
            if width == 0 || height == 0 {
                continue;
            }
            let dirty_pixels = width
                .checked_mul(height)
                .ok_or(CaptureError::BufferOverflow)?;
            total_dirty_pixels = total_dirty_pixels
                .checked_add(dirty_pixels)
                .ok_or(CaptureError::BufferOverflow)?;
            converted = converted
                .checked_add(1)
                .ok_or(CaptureError::BufferOverflow)?;
        }
        (converted, total_dirty_pixels)
    };
    if converted == 0 || total_dirty_pixels == 0 {
        return Ok(0);
    }

    let should_parallelize = convert::should_parallelize_work(
        total_dirty_pixels,
        DIRTY_RECT_PARALLEL_MIN_PIXELS,
        DIRTY_RECT_PARALLEL_MIN_CHUNK_PIXELS,
        DIRTY_RECT_PARALLEL_MAX_WORKERS,
    );
    let use_bgra_batch_kernel = format == SurfacePixelFormat::Bgra8;
    let can_parallel = converted >= DIRTY_RECT_PARALLEL_MIN_RECTS && should_parallelize;

    if can_parallel {
        use rayon::prelude::*;

        let src_addr = src_base as usize;
        let dst_addr = dst_base as usize;
        let bgra_kernel = if use_bgra_batch_kernel {
            // Dirty-rect destinations can start at arbitrary x offsets, so we
            // force the temporal kernel here and avoid NT alignment/fence
            // assumptions that only hold for per-rect kernel selection.
            Some(convert::select_bgra_dirty_rect_kernel(
                dst_base as *const u8,
                dst_pitch,
                DIRTY_RECT_BATCH_FORCE_TEMPORAL_PIXELS,
                false,
                force_opaque_alpha,
            ))
        } else {
            None
        };
        convert::with_conversion_pool(DIRTY_RECT_PARALLEL_MAX_WORKERS, || {
            dirty_rects.par_iter().for_each(|rect| {
                let width = rect.width as usize;
                let height = rect.height as usize;
                if width == 0 || height == 0 {
                    return;
                }

                let x = rect.x as usize;
                let y = rect.y as usize;
                let src_offset = y * src_pitch + x * src_bytes_per_pixel;
                let dst_offset = (dst_origin_y + y) * dst_pitch + (dst_origin_x + x) * 4;
                unsafe {
                    if let Some(kernel) = bgra_kernel {
                        convert::convert_bgra_rows_with_kernel_unchecked(
                            kernel,
                            (src_addr + src_offset) as *const u8,
                            src_pitch,
                            (dst_addr + dst_offset) as *mut u8,
                            dst_pitch,
                            width,
                            height,
                        );
                    } else {
                        converter.convert_rows_unchecked(
                            (src_addr + src_offset) as *const u8,
                            src_pitch,
                            (dst_addr + dst_offset) as *mut u8,
                            dst_pitch,
                            width,
                            height,
                        );
                    }
                }
            });
        });
        return Ok(converted);
    }

    let allow_inner_parallel = converted == 1;
    let serial_bgra_kernel = if use_bgra_batch_kernel && !allow_inner_parallel {
        // See `bgra_kernel` above: batch dispatch intentionally sticks to the
        // temporal kernel because each dirty rect can have a distinct x offset.
        Some(convert::select_bgra_dirty_rect_kernel(
            dst_base as *const u8,
            dst_pitch,
            DIRTY_RECT_BATCH_FORCE_TEMPORAL_PIXELS,
            true,
            force_opaque_alpha,
        ))
    } else {
        None
    };
    for rect in dirty_rects {
        let width = rect.width as usize;
        let height = rect.height as usize;
        if width == 0 || height == 0 {
            continue;
        }

        let x = rect.x as usize;
        let y = rect.y as usize;
        let src_offset = y * src_pitch + x * src_bytes_per_pixel;
        let dst_offset = (dst_origin_y + y) * dst_pitch + (dst_origin_x + x) * 4;
        unsafe {
            if let Some(kernel) = serial_bgra_kernel {
                convert::convert_bgra_rows_with_kernel_unchecked(
                    kernel,
                    src_base.add(src_offset),
                    src_pitch,
                    dst_base.add(dst_offset),
                    dst_pitch,
                    width,
                    height,
                );
            } else if allow_inner_parallel {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_base.add(src_offset),
                    src_pitch,
                    dst_base.add(dst_offset),
                    dst_pitch,
                    width,
                    height,
                );
            } else {
                converter.convert_rows_unchecked(
                    src_base.add(src_offset),
                    src_pitch,
                    dst_base.add(dst_offset),
                    dst_pitch,
                    width,
                    height,
                );
            }
        }
    }

    if let Some(kernel) = serial_bgra_kernel {
        convert::finalize_bgra_dirty_rect_kernel(kernel);
    }

    Ok(converted)
}

pub(crate) fn ensure_staging_texture<'a>(
    device: &ID3D11Device,
    staging: &'a mut Option<ID3D11Texture2D>,
    src: &D3D11_TEXTURE2D_DESC,
    sample_desc: StagingSampleDesc,
    create_context: &'static str,
) -> CaptureResult<&'a ID3D11Texture2D> {
    let needs_new_staging = match staging {
        Some(existing) => {
            let mut desc = D3D11_TEXTURE2D_DESC::default();
            unsafe { existing.GetDesc(&mut desc) };
            desc.Width != src.Width || desc.Height != src.Height || desc.Format != src.Format
        }
        None => true,
    };

    if needs_new_staging {
        let sample_desc = match sample_desc {
            StagingSampleDesc::SingleSample => DXGI_SAMPLE_DESC {
                Count: 1,
                Quality: 0,
            },
        };

        let desc = D3D11_TEXTURE2D_DESC {
            Width: src.Width,
            Height: src.Height,
            MipLevels: 1,
            ArraySize: 1,
            Format: src.Format,
            SampleDesc: sample_desc,
            Usage: D3D11_USAGE_STAGING,
            BindFlags: Default::default(),
            CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
            MiscFlags: Default::default(),
        };

        let mut texture: Option<ID3D11Texture2D> = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture)) }
            .context(create_context)
            .map_err(CaptureError::platform)?;
        *staging = texture;
    }

    Ok(staging.as_ref().unwrap())
}
/// Build a `D3D11_TEXTURE2D_DESC` sized for a region blit, inheriting format
/// and other properties from the full-frame source descriptor.
pub(crate) fn region_desc_for_blit(
    source_desc: &D3D11_TEXTURE2D_DESC,
    blit: CaptureBlitRegion,
) -> D3D11_TEXTURE2D_DESC {
    let mut region_desc = *source_desc;
    region_desc.Width = blit.width;
    region_desc.Height = blit.height;
    region_desc.MipLevels = 1;
    region_desc.ArraySize = 1;
    region_desc.SampleDesc.Count = 1;
    region_desc.SampleDesc.Quality = 0;
    region_desc
}

pub(crate) fn copy_mapped_surface_to_frame(
    frame: &mut Frame,
    desc: &D3D11_TEXTURE2D_DESC,
    mapped: &D3D11_MAPPED_SUBRESOURCE,
    options: SurfaceConversionOptions,
) -> CaptureResult<()> {
    let width_u32 = desc.Width;
    let height_u32 = desc.Height;
    frame.ensure_rgba_capacity(width_u32, height_u32)?;
    mark_frame_srgb(frame);

    let width = usize::try_from(width_u32).map_err(|_| CaptureError::BufferOverflow)?;
    let height = usize::try_from(height_u32).map_err(|_| CaptureError::BufferOverflow)?;

    let src_pitch = mapped.RowPitch as usize;
    let src_base = mapped.pData as *const u8;
    let format = source_surface_format(desc.Format)
        .ok_or_else(|| CaptureError::UnsupportedFormat(format!("{:?}", desc.Format)))?;

    let src_row_len = source_row_bytes(format, width)?;
    src_pitch
        .checked_mul(height.saturating_sub(1))
        .and_then(|base| base.checked_add(src_row_len))
        .ok_or(CaptureError::BufferOverflow)?;

    let dst_pitch = width.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
    unsafe {
        convert::convert_surface_to_rgba_unchecked(
            format,
            SurfaceLayout::new(
                src_base,
                src_pitch,
                frame.as_mut_rgba_ptr(),
                dst_pitch,
                width,
                height,
            ),
            options,
        );
    }
    if options.force_opaque_alpha && !convert::conversion_fuses_opaque_alpha(format, options) {
        force_frame_alpha_opaque(frame);
    }
    Ok(())
}

/// Map an already-populated staging texture and convert its contents into
/// the output frame. This assumes the GPU copy into `staging` has already
/// been submitted by the caller.
///
/// When `staging_resource` is `Some`, the pre-cached `ID3D11Resource` is
/// used directly, avoiding a COM `QueryInterface` (`cast()`) on every call.
fn map_staging_to_frame_internal(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    options: SurfaceConversionOptions,
    map_context: &'static str,
    use_spin_map: bool,
) -> CaptureResult<()> {
    let owned_resource;
    let resource = match staging_resource {
        Some(r) => r,
        None => {
            owned_resource = staging
                .cast::<ID3D11Resource>()
                .context("failed to cast staging texture to ID3D11Resource")
                .map_err(CaptureError::platform)?;
            &owned_resource
        }
    };

    let mapped = if use_spin_map {
        map_resource_read_with_spin(context, resource, map_context)?
    } else {
        map_resource_read_blocking(context, resource, map_context)?
    };

    let result = copy_mapped_surface_to_frame(frame, desc, &mapped, options);
    unsafe {
        context.Unmap(resource, 0);
    }
    result
}

pub(crate) fn map_staging_to_frame(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<()> {
    map_staging_to_frame_internal(
        context,
        staging,
        staging_resource,
        desc,
        frame,
        options,
        map_context,
        true,
    )
}

/// Screenshot-style single-shot variant.
///
/// We still prefer a short DO_NOT_WAIT spin first because the GPU copy often
/// completes within a handful of polls; falling back to blocking `Map()` keeps
/// worst-case behavior unchanged.
pub(crate) fn map_staging_to_frame_blocking(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<()> {
    map_staging_to_frame_internal(
        context,
        staging,
        staging_resource,
        desc,
        frame,
        options,
        map_context,
        true,
    )
}

/// Map a populated staging texture and convert a source sub-rectangle into
/// the destination frame at `blit.dst_x`/`blit.dst_y`.
fn map_staging_rect_to_frame_internal(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    blit: CaptureBlitRegion,
    options: SurfaceConversionOptions,
    map_context: &'static str,
    use_spin_map: bool,
) -> CaptureResult<()> {
    if blit.width == 0 || blit.height == 0 {
        return Ok(());
    }
    mark_frame_srgb(frame);

    let src_width = usize::try_from(desc.Width).map_err(|_| CaptureError::BufferOverflow)?;
    let src_height = usize::try_from(desc.Height).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_width = usize::try_from(frame.width()).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_height = usize::try_from(frame.height()).map_err(|_| CaptureError::BufferOverflow)?;

    let src_x = usize::try_from(blit.src_x).map_err(|_| CaptureError::BufferOverflow)?;
    let src_y = usize::try_from(blit.src_y).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_x = usize::try_from(blit.dst_x).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_y = usize::try_from(blit.dst_y).map_err(|_| CaptureError::BufferOverflow)?;
    let copy_w = usize::try_from(blit.width).map_err(|_| CaptureError::BufferOverflow)?;
    let copy_h = usize::try_from(blit.height).map_err(|_| CaptureError::BufferOverflow)?;

    let src_right = src_x
        .checked_add(copy_w)
        .ok_or(CaptureError::BufferOverflow)?;
    let src_bottom = src_y
        .checked_add(copy_h)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_right = dst_x
        .checked_add(copy_w)
        .ok_or(CaptureError::BufferOverflow)?;
    let dst_bottom = dst_y
        .checked_add(copy_h)
        .ok_or(CaptureError::BufferOverflow)?;
    if src_right > src_width
        || src_bottom > src_height
        || dst_right > dst_width
        || dst_bottom > dst_height
    {
        return Err(CaptureError::BufferOverflow);
    }

    let owned_resource;
    let resource = match staging_resource {
        Some(r) => r,
        None => {
            owned_resource = staging
                .cast::<ID3D11Resource>()
                .context("failed to cast staging texture to ID3D11Resource")
                .map_err(CaptureError::platform)?;
            &owned_resource
        }
    };

    let mapped = if use_spin_map {
        map_resource_read_with_spin(context, resource, map_context)?
    } else {
        map_resource_read_blocking(context, resource, map_context)?
    };

    let convert_result = (|| -> CaptureResult<()> {
        let src_pitch = mapped.RowPitch as usize;
        let src_base = mapped.pData as *const u8;
        let format = source_surface_format(desc.Format)
            .ok_or_else(|| CaptureError::UnsupportedFormat(format!("{:?}", desc.Format)))?;
        let src_row_len = source_row_bytes(format, src_width)?;
        src_pitch
            .checked_mul(src_height.saturating_sub(1))
            .and_then(|base| base.checked_add(src_row_len))
            .ok_or(CaptureError::BufferOverflow)?;

        let src_bytes_per_pixel = source_bytes_per_pixel(format);
        let src_copy_row_bytes = source_row_bytes(format, copy_w)?;
        let src_end_in_row = src_x
            .checked_mul(src_bytes_per_pixel)
            .and_then(|offset| offset.checked_add(src_copy_row_bytes))
            .ok_or(CaptureError::BufferOverflow)?;
        if src_end_in_row > src_pitch {
            return Err(CaptureError::BufferOverflow);
        }

        let dst_pitch = dst_width
            .checked_mul(4)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_copy_row_bytes = copy_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
        let dst_end_in_row = dst_x
            .checked_mul(4)
            .and_then(|offset| offset.checked_add(dst_copy_row_bytes))
            .ok_or(CaptureError::BufferOverflow)?;
        if dst_end_in_row > dst_pitch {
            return Err(CaptureError::BufferOverflow);
        }

        let src_offset = src_y
            .checked_mul(src_pitch)
            .and_then(|base| {
                src_x
                    .checked_mul(src_bytes_per_pixel)
                    .and_then(|xoff| base.checked_add(xoff))
            })
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_offset = dst_y
            .checked_mul(dst_pitch)
            .and_then(|base| dst_x.checked_mul(4).and_then(|xoff| base.checked_add(xoff)))
            .ok_or(CaptureError::BufferOverflow)?;

        unsafe {
            convert::convert_surface_to_rgba_unchecked(
                format,
                SurfaceLayout::new(
                    src_base.add(src_offset),
                    src_pitch,
                    frame.as_mut_rgba_ptr().add(dst_offset),
                    dst_pitch,
                    copy_w,
                    copy_h,
                ),
                options,
            );
        }
        Ok(())
    })();

    unsafe {
        context.Unmap(resource, 0);
    }
    if convert_result.is_ok()
        && options.force_opaque_alpha
        && source_surface_format(desc.Format)
            .is_some_and(|format| !convert::conversion_fuses_opaque_alpha(format, options))
    {
        force_frame_alpha_opaque_in_rects(
            frame,
            &[DirtyRect {
                x: 0,
                y: 0,
                width: blit.width,
                height: blit.height,
            }],
            blit.dst_x,
            blit.dst_y,
        )?;
    }
    convert_result
}

pub(crate) fn map_staging_rect_to_frame(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    blit: CaptureBlitRegion,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<()> {
    map_staging_rect_to_frame_internal(
        context,
        staging,
        staging_resource,
        desc,
        frame,
        blit,
        options,
        map_context,
        true,
    )
}

pub(crate) fn map_staging_rect_to_frame_blocking(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    blit: CaptureBlitRegion,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<()> {
    map_staging_rect_to_frame_internal(
        context,
        staging,
        staging_resource,
        desc,
        frame,
        blit,
        options,
        map_context,
        false,
    )
}

/// Map an already-populated staging texture and apply only the provided
/// dirty rectangles to the output frame.
///
/// Returns the number of dirty rectangles that were actually converted.
pub(crate) fn map_staging_dirty_rects_to_frame(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    dirty_rects: &[DirtyRect],
    dirty_rects_non_overlapping: bool,
    hints: DirtyRectConversionHints,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<usize> {
    frame.ensure_rgba_capacity(desc.Width, desc.Height)?;
    mark_frame_srgb(frame);
    map_staging_dirty_rects_to_frame_with_offset(
        context,
        staging,
        staging_resource,
        desc,
        frame,
        dirty_rects,
        0,
        0,
        dirty_rects_non_overlapping,
        hints,
        options,
        map_context,
    )
}

/// Map an already-populated staging texture and apply dirty rectangles to the
/// output frame at an offset destination origin.
///
/// Dirty rectangles are in source texture coordinates. They are written to
/// `frame` at `(dst_origin_x + rect.x, dst_origin_y + rect.y)`.
pub(crate) fn map_staging_dirty_rects_to_frame_with_offset(
    context: &ID3D11DeviceContext,
    staging: &ID3D11Texture2D,
    staging_resource: Option<&ID3D11Resource>,
    desc: &D3D11_TEXTURE2D_DESC,
    frame: &mut Frame,
    dirty_rects: &[DirtyRect],
    dst_origin_x: u32,
    dst_origin_y: u32,
    dirty_rects_non_overlapping: bool,
    hints: DirtyRectConversionHints,
    options: SurfaceConversionOptions,
    map_context: &'static str,
) -> CaptureResult<usize> {
    if dirty_rects.is_empty() {
        return Ok(0);
    }
    mark_frame_srgb(frame);

    let width_u32 = desc.Width;
    let height_u32 = desc.Height;
    let width = usize::try_from(width_u32).map_err(|_| CaptureError::BufferOverflow)?;
    let height = usize::try_from(height_u32).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_origin_x = usize::try_from(dst_origin_x).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_origin_y = usize::try_from(dst_origin_y).map_err(|_| CaptureError::BufferOverflow)?;

    let dst_width_u32 = frame.width();
    let dst_height_u32 = frame.height();
    let dst_width = usize::try_from(dst_width_u32).map_err(|_| CaptureError::BufferOverflow)?;
    let dst_height = usize::try_from(dst_height_u32).map_err(|_| CaptureError::BufferOverflow)?;

    let owned_resource;
    let resource = match staging_resource {
        Some(r) => r,
        None => {
            owned_resource = staging
                .cast::<ID3D11Resource>()
                .context("failed to cast staging texture to ID3D11Resource")
                .map_err(CaptureError::platform)?;
            &owned_resource
        }
    };

    let mapped = map_resource_read_with_spin(context, resource, map_context)?;

    let convert_result = (|| -> CaptureResult<usize> {
        let src_pitch = mapped.RowPitch as usize;
        let src_base = mapped.pData as *const u8;
        let format = source_surface_format(desc.Format)
            .ok_or_else(|| CaptureError::UnsupportedFormat(format!("{:?}", desc.Format)))?;
        let src_row_len = source_row_bytes(format, width)?;
        src_pitch
            .checked_mul(height.saturating_sub(1))
            .and_then(|base| base.checked_add(src_row_len))
            .ok_or(CaptureError::BufferOverflow)?;

        let src_bytes_per_pixel = source_bytes_per_pixel(format);
        let dst_pitch = dst_width
            .checked_mul(4)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_base = frame.as_mut_rgba_ptr();
        let converter = convert::SurfaceRowConverter::new(format, options);
        let trusted_fastpath = dirty_rects_non_overlapping
            && (hints.trusted_bounds
                || dirty_rects_fit_bounds(
                    dirty_rects,
                    width_u32,
                    height_u32,
                    dst_origin_x as u32,
                    dst_origin_y as u32,
                    dst_width_u32,
                    dst_height_u32,
                ));

        if trusted_fastpath {
            // SAFETY: `trusted_fastpath` guarantees all dirty rectangles are
            // in-bounds for both source and destination surfaces.
            return unsafe {
                convert_dirty_rects_trusted_direct_unchecked(
                    format,
                    converter,
                    src_base,
                    src_pitch,
                    src_bytes_per_pixel,
                    dst_base,
                    dst_pitch,
                    dirty_rects,
                    dst_origin_x,
                    dst_origin_y,
                    hints,
                    options.force_opaque_alpha,
                )
            };
        }

        if dirty_rects.len() < DIRTY_RECT_PARALLEL_MIN_RECTS {
            let allow_inner_parallel = dirty_rects.len() == 1;
            let mut converted = 0usize;
            for rect in dirty_rects {
                let item = if trusted_fastpath {
                    // SAFETY: trusted mode is reserved for normalized,
                    // in-bounds dirty rect lists produced by capture backends.
                    unsafe {
                        build_dirty_rect_work_item_trusted(
                            *rect,
                            dst_origin_x,
                            dst_origin_y,
                            src_pitch,
                            src_bytes_per_pixel,
                            dst_pitch,
                        )
                    }
                } else {
                    build_dirty_rect_work_item(
                        rect,
                        width,
                        height,
                        dst_origin_x,
                        dst_origin_y,
                        dst_width,
                        dst_height,
                        src_pitch,
                        src_bytes_per_pixel,
                        dst_pitch,
                    )?
                };
                let Some(item) = item else {
                    continue;
                };
                unsafe {
                    if allow_inner_parallel {
                        converter.convert_rows_maybe_parallel_unchecked(
                            src_base.add(item.src_offset),
                            src_pitch,
                            dst_base.add(item.dst_offset),
                            dst_pitch,
                            item.width,
                            item.height,
                        );
                    } else {
                        converter.convert_rows_unchecked(
                            src_base.add(item.src_offset),
                            src_pitch,
                            dst_base.add(item.dst_offset),
                            dst_pitch,
                            item.width,
                            item.height,
                        );
                    }
                }
                converted = converted.saturating_add(1);
            }
            return Ok(converted);
        }

        DIRTY_WORK_ITEMS_SCRATCH.with(|scratch| -> CaptureResult<usize> {
            let mut work_items = scratch.borrow_mut();
            work_items.clear();
            let additional_capacity = dirty_rects.len().saturating_sub(work_items.capacity());
            if additional_capacity > 0 {
                work_items.reserve(additional_capacity);
            }

            let mut total_dirty_pixels = 0usize;
            if trusted_fastpath {
                for rect in dirty_rects {
                    // SAFETY: trusted mode is reserved for normalized,
                    // in-bounds dirty rect lists produced by capture backends.
                    let Some(item) = (unsafe {
                        build_dirty_rect_work_item_trusted(
                            *rect,
                            dst_origin_x,
                            dst_origin_y,
                            src_pitch,
                            src_bytes_per_pixel,
                            dst_pitch,
                        )
                    }) else {
                        continue;
                    };
                    let dirty_pixels = item
                        .width
                        .checked_mul(item.height)
                        .ok_or(CaptureError::BufferOverflow)?;
                    total_dirty_pixels = total_dirty_pixels
                        .checked_add(dirty_pixels)
                        .ok_or(CaptureError::BufferOverflow)?;
                    work_items.push(item);
                }
            } else {
                for rect in dirty_rects {
                    let Some(item) = build_dirty_rect_work_item(
                        rect,
                        width,
                        height,
                        dst_origin_x,
                        dst_origin_y,
                        dst_width,
                        dst_height,
                        src_pitch,
                        src_bytes_per_pixel,
                        dst_pitch,
                    )?
                    else {
                        continue;
                    };

                    let dirty_pixels = item
                        .width
                        .checked_mul(item.height)
                        .ok_or(CaptureError::BufferOverflow)?;
                    total_dirty_pixels = total_dirty_pixels
                        .checked_add(dirty_pixels)
                        .ok_or(CaptureError::BufferOverflow)?;
                    work_items.push(item);
                }
            }

            if work_items.is_empty() {
                return Ok(0);
            }

            let should_parallelize = convert::should_parallelize_work(
                total_dirty_pixels,
                DIRTY_RECT_PARALLEL_MIN_PIXELS,
                DIRTY_RECT_PARALLEL_MIN_CHUNK_PIXELS,
                DIRTY_RECT_PARALLEL_MAX_WORKERS,
            );
            let work_items_slice = &work_items[..];
            let non_overlapping = if dirty_rects_non_overlapping {
                debug_assert!(work_items_non_overlapping(work_items_slice));
                true
            } else {
                work_items_non_overlapping(work_items_slice)
            };
            let can_parallel = work_items_slice.len() >= DIRTY_RECT_PARALLEL_MIN_RECTS
                && should_parallelize
                && non_overlapping;

            if can_parallel {
                use rayon::prelude::*;

                let src_addr = src_base as usize;
                let dst_addr = dst_base as usize;
                convert::with_conversion_pool(DIRTY_RECT_PARALLEL_MAX_WORKERS, || {
                    work_items_slice.par_iter().for_each(|item| unsafe {
                        converter.convert_rows_unchecked(
                            (src_addr + item.src_offset) as *const u8,
                            src_pitch,
                            (dst_addr + item.dst_offset) as *mut u8,
                            dst_pitch,
                            item.width,
                            item.height,
                        );
                    });
                });
            } else {
                let allow_inner_parallel = work_items_slice.len() == 1;
                for item in work_items_slice {
                    unsafe {
                        if allow_inner_parallel {
                            converter.convert_rows_maybe_parallel_unchecked(
                                src_base.add(item.src_offset),
                                src_pitch,
                                dst_base.add(item.dst_offset),
                                dst_pitch,
                                item.width,
                                item.height,
                            );
                        } else {
                            converter.convert_rows_unchecked(
                                src_base.add(item.src_offset),
                                src_pitch,
                                dst_base.add(item.dst_offset),
                                dst_pitch,
                                item.width,
                                item.height,
                            );
                        }
                    }
                }
            }

            Ok(work_items_slice.len())
        })
    })();

    unsafe {
        context.Unmap(resource, 0);
    }
    match convert_result {
        Ok(converted)
            if converted > 0
                && options.force_opaque_alpha
                && source_surface_format(desc.Format).is_some_and(|format| {
                    !convert::conversion_fuses_opaque_alpha(format, options)
                }) =>
        {
            force_frame_alpha_opaque_in_rects(
                frame,
                dirty_rects,
                u32::try_from(dst_origin_x).map_err(|_| CaptureError::BufferOverflow)?,
                u32::try_from(dst_origin_y).map_err(|_| CaptureError::BufferOverflow)?,
            )?;
            Ok(converted)
        }
        other => other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn item(x: usize, y: usize, width: usize, height: usize) -> DirtyRectWorkItem {
        DirtyRectWorkItem {
            src_offset: 0,
            dst_offset: 0,
            x,
            y,
            width,
            height,
        }
    }

    #[test]
    fn work_items_overlap_detects_intersection() {
        let a = item(10, 10, 40, 30);
        let b = item(30, 20, 20, 20);
        assert!(work_items_overlap(a, b));
    }

    #[test]
    fn work_items_overlap_rejects_edge_touch() {
        let a = item(10, 10, 20, 20);
        let b = item(30, 10, 20, 20);
        assert!(!work_items_overlap(a, b));
    }

    #[test]
    fn work_items_non_overlapping_detects_collision() {
        let work_items = vec![item(0, 0, 32, 32), item(31, 4, 16, 16), item(80, 5, 8, 8)];
        assert!(!work_items_non_overlapping(&work_items));
    }

    #[test]
    fn work_items_non_overlapping_accepts_disjoint_list() {
        let work_items = vec![item(0, 0, 16, 16), item(20, 0, 12, 16), item(0, 20, 8, 8)];
        assert!(work_items_non_overlapping(&work_items));
    }

    #[test]
    fn build_dirty_rect_work_item_clamps_to_source_bounds() {
        let rect = DirtyRect {
            x: 1910,
            y: 1075,
            width: 32,
            height: 20,
        };
        let item = build_dirty_rect_work_item(&rect, 1920, 1080, 0, 0, 1920, 1080, 7680, 4, 7680)
            .expect("work item build failed")
            .expect("expected dirty work item");
        assert_eq!(item.width, 10);
        assert_eq!(item.height, 5);
        assert_eq!(item.x, 1910);
        assert_eq!(item.y, 1075);
    }

    #[test]
    fn build_dirty_rect_work_item_rejects_destination_overflow() {
        let rect = DirtyRect {
            x: 40,
            y: 10,
            width: 32,
            height: 16,
        };
        let result = build_dirty_rect_work_item(&rect, 128, 128, 100, 0, 128, 128, 512, 4, 512);
        assert!(matches!(result, Err(CaptureError::BufferOverflow)));
    }

    #[test]
    fn dirty_rects_fit_bounds_accepts_valid_rects() {
        let rects = vec![
            DirtyRect {
                x: 4,
                y: 8,
                width: 12,
                height: 16,
            },
            DirtyRect {
                x: 64,
                y: 20,
                width: 24,
                height: 12,
            },
        ];
        assert!(dirty_rects_fit_bounds(&rects, 128, 96, 10, 6, 192, 128));
    }

    #[test]
    fn dirty_rects_fit_bounds_rejects_destination_overflow() {
        let rects = vec![DirtyRect {
            x: 120,
            y: 0,
            width: 16,
            height: 4,
        }];
        assert!(!dirty_rects_fit_bounds(&rects, 256, 64, 80, 0, 200, 100));
    }

    #[test]
    fn trusted_dirty_rect_work_item_matches_checked_builder() {
        let rect = DirtyRect {
            x: 25,
            y: 40,
            width: 10,
            height: 12,
        };
        let checked = build_dirty_rect_work_item(&rect, 256, 256, 3, 7, 400, 400, 1024, 4, 1600)
            .expect("checked builder failed")
            .expect("checked builder returned None");
        let trusted = unsafe { build_dirty_rect_work_item_trusted(rect, 3, 7, 1024, 4, 1600) }
            .expect("trusted builder returned None");
        assert_eq!(checked.src_offset, trusted.src_offset);
        assert_eq!(checked.dst_offset, trusted.dst_offset);
        assert_eq!(checked.width, trusted.width);
        assert_eq!(checked.height, trusted.height);
    }

    #[test]
    fn trusted_direct_dirty_rect_conversion_matches_work_item_path() {
        let src_width = 160usize;
        let src_height = 96usize;
        let src_pitch = src_width * 4 + 64;
        let src_len = src_pitch * (src_height - 1) + src_width * 4;
        let mut src = vec![0u8; src_len];
        for (idx, byte) in src.iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_mul(29).wrapping_add(7);
        }

        let dst_origin_x = 7usize;
        let dst_origin_y = 11usize;
        let dst_width = 220usize;
        let dst_height = 170usize;
        let dst_pitch = dst_width * 4;
        let dst_len = dst_pitch * dst_height;

        let dirty_rects = vec![
            DirtyRect {
                x: 2,
                y: 3,
                width: 24,
                height: 12,
            },
            DirtyRect {
                x: 40,
                y: 10,
                width: 20,
                height: 8,
            },
            DirtyRect {
                x: 90,
                y: 22,
                width: 30,
                height: 10,
            },
            DirtyRect {
                x: 18,
                y: 48,
                width: 42,
                height: 16,
            },
            DirtyRect {
                x: 130,
                y: 60,
                width: 18,
                height: 14,
            },
            DirtyRect {
                x: 12,
                y: 80,
                width: 0,
                height: 4,
            },
        ];

        assert!(dirty_rects_fit_bounds(
            &dirty_rects,
            src_width as u32,
            src_height as u32,
            dst_origin_x as u32,
            dst_origin_y as u32,
            dst_width as u32,
            dst_height as u32
        ));

        let converter = convert::SurfaceRowConverter::new(
            SurfacePixelFormat::Bgra8,
            SurfaceConversionOptions {
                hdr_to_sdr: None,
                ..SurfaceConversionOptions::default()
            },
        );

        let mut expected = vec![0u8; dst_len];
        let mut expected_converted = 0usize;
        for rect in &dirty_rects {
            let item = build_dirty_rect_work_item(
                rect,
                src_width,
                src_height,
                dst_origin_x,
                dst_origin_y,
                dst_width,
                dst_height,
                src_pitch,
                4,
                dst_pitch,
            )
            .expect("checked work-item conversion failed");
            let Some(item) = item else {
                continue;
            };
            expected_converted = expected_converted.saturating_add(1);
            unsafe {
                converter.convert_rows_unchecked(
                    src.as_ptr().add(item.src_offset),
                    src_pitch,
                    expected.as_mut_ptr().add(item.dst_offset),
                    dst_pitch,
                    item.width,
                    item.height,
                );
            }
        }

        let mut direct = vec![0u8; dst_len];
        let converted = unsafe {
            convert_dirty_rects_trusted_direct_unchecked(
                SurfacePixelFormat::Bgra8,
                converter,
                src.as_ptr(),
                src_pitch,
                4,
                direct.as_mut_ptr(),
                dst_pitch,
                &dirty_rects,
                dst_origin_x,
                dst_origin_y,
                DirtyRectConversionHints::default(),
                false,
            )
        }
        .expect("trusted direct conversion failed");

        assert_eq!(converted, expected_converted);
        assert_eq!(direct, expected);
    }

    #[test]
    fn trusted_direct_dirty_rect_conversion_recovers_from_invalid_hints() {
        let src_width = 64usize;
        let src_height = 48usize;
        let src_pitch = src_width * 4;
        let src_len = src_pitch * src_height;
        let mut src = vec![0u8; src_len];
        for (idx, byte) in src.iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_mul(19).wrapping_add(3);
        }

        let dst_pitch = src_pitch;
        let dst_len = src_len;
        let dirty_rects = vec![
            DirtyRect {
                x: 4,
                y: 6,
                width: 12,
                height: 10,
            },
            DirtyRect {
                x: 24,
                y: 14,
                width: 16,
                height: 12,
            },
        ];

        let converter = convert::SurfaceRowConverter::new(
            SurfacePixelFormat::Bgra8,
            SurfaceConversionOptions {
                hdr_to_sdr: None,
                ..SurfaceConversionOptions::default()
            },
        );

        let mut baseline = vec![0u8; dst_len];
        let baseline_converted = unsafe {
            convert_dirty_rects_trusted_direct_unchecked(
                SurfacePixelFormat::Bgra8,
                converter,
                src.as_ptr(),
                src_pitch,
                4,
                baseline.as_mut_ptr(),
                dst_pitch,
                &dirty_rects,
                0,
                0,
                DirtyRectConversionHints::default(),
                false,
            )
        }
        .expect("baseline conversion failed");

        let mut hinted = vec![0u8; dst_len];
        let hinted_converted = unsafe {
            convert_dirty_rects_trusted_direct_unchecked(
                SurfacePixelFormat::Bgra8,
                converter,
                src.as_ptr(),
                src_pitch,
                4,
                hinted.as_mut_ptr(),
                dst_pitch,
                &dirty_rects,
                0,
                0,
                DirtyRectConversionHints {
                    trusted_bounds: true,
                    non_empty_rects: Some(dirty_rects.len() + 1),
                    total_dirty_pixels: Some(0),
                },
                false,
            )
        }
        .expect("hinted conversion failed");

        assert_eq!(hinted_converted, baseline_converted);
        assert_eq!(hinted, baseline);
    }

    fn row_major_rects(
        start_x: u32,
        start_y: u32,
        cols: u32,
        rows: u32,
        rect_w: u32,
        rect_h: u32,
        gap_x: u32,
        gap_y: u32,
        limit: usize,
    ) -> Vec<DirtyRect> {
        let mut out = Vec::with_capacity(limit);
        for row in 0..rows {
            for col in 0..cols {
                if out.len() == limit {
                    return out;
                }
                let x = start_x + col * (rect_w + gap_x);
                let y = start_y + row * (rect_h + gap_y);
                out.push(DirtyRect {
                    x,
                    y,
                    width: rect_w,
                    height: rect_h,
                });
            }
        }
        out
    }

    fn dirty_rect_stats(rects: &[DirtyRect]) -> (usize, usize) {
        let mut non_empty = 0usize;
        let mut dirty_pixels = 0usize;
        for rect in rects {
            let width = rect.width as usize;
            let height = rect.height as usize;
            if width == 0 || height == 0 {
                continue;
            }
            non_empty = non_empty.saturating_add(1);
            dirty_pixels = dirty_pixels.saturating_add(width.saturating_mul(height));
        }
        (non_empty, dirty_pixels)
    }

    unsafe fn convert_dirty_rects_trusted_direct_baseline_unchecked(
        converter: convert::SurfaceRowConverter,
        src_base: *const u8,
        src_pitch: usize,
        src_bytes_per_pixel: usize,
        dst_base: *mut u8,
        dst_pitch: usize,
        dirty_rects: &[DirtyRect],
        dst_origin_x: usize,
        dst_origin_y: usize,
        hints: DirtyRectConversionHints,
    ) -> CaptureResult<usize> {
        let consistent_hints = match (hints.non_empty_rects, hints.total_dirty_pixels) {
            (Some(non_empty_rects), Some(total_dirty_pixels))
                if non_empty_rects <= dirty_rects.len()
                    && ((non_empty_rects == 0 && total_dirty_pixels == 0)
                        || (non_empty_rects > 0 && total_dirty_pixels > 0)) =>
            {
                Some((non_empty_rects, total_dirty_pixels))
            }
            _ => None,
        };

        let (converted, total_dirty_pixels) = if let Some(pair) = consistent_hints {
            pair
        } else {
            let mut converted = 0usize;
            let mut total_dirty_pixels = 0usize;
            for rect in dirty_rects {
                let width = rect.width as usize;
                let height = rect.height as usize;
                if width == 0 || height == 0 {
                    continue;
                }
                let dirty_pixels = width
                    .checked_mul(height)
                    .ok_or(CaptureError::BufferOverflow)?;
                total_dirty_pixels = total_dirty_pixels
                    .checked_add(dirty_pixels)
                    .ok_or(CaptureError::BufferOverflow)?;
                converted = converted
                    .checked_add(1)
                    .ok_or(CaptureError::BufferOverflow)?;
            }
            (converted, total_dirty_pixels)
        };
        if converted == 0 || total_dirty_pixels == 0 {
            return Ok(0);
        }

        let should_parallelize = convert::should_parallelize_work(
            total_dirty_pixels,
            DIRTY_RECT_PARALLEL_MIN_PIXELS,
            DIRTY_RECT_PARALLEL_MIN_CHUNK_PIXELS,
            DIRTY_RECT_PARALLEL_MAX_WORKERS,
        );
        let can_parallel = converted >= DIRTY_RECT_PARALLEL_MIN_RECTS && should_parallelize;

        if can_parallel {
            use rayon::prelude::*;

            let src_addr = src_base as usize;
            let dst_addr = dst_base as usize;
            convert::with_conversion_pool(DIRTY_RECT_PARALLEL_MAX_WORKERS, || {
                dirty_rects.par_iter().for_each(|rect| {
                    let width = rect.width as usize;
                    let height = rect.height as usize;
                    if width == 0 || height == 0 {
                        return;
                    }

                    let x = rect.x as usize;
                    let y = rect.y as usize;
                    let src_offset = y * src_pitch + x * src_bytes_per_pixel;
                    let dst_offset = (dst_origin_y + y) * dst_pitch + (dst_origin_x + x) * 4;
                    unsafe {
                        converter.convert_rows_unchecked(
                            (src_addr + src_offset) as *const u8,
                            src_pitch,
                            (dst_addr + dst_offset) as *mut u8,
                            dst_pitch,
                            width,
                            height,
                        );
                    }
                });
            });
            return Ok(converted);
        }

        let allow_inner_parallel = converted == 1;
        for rect in dirty_rects {
            let width = rect.width as usize;
            let height = rect.height as usize;
            if width == 0 || height == 0 {
                continue;
            }

            let x = rect.x as usize;
            let y = rect.y as usize;
            let src_offset = y * src_pitch + x * src_bytes_per_pixel;
            let dst_offset = (dst_origin_y + y) * dst_pitch + (dst_origin_x + x) * 4;
            unsafe {
                if allow_inner_parallel {
                    converter.convert_rows_maybe_parallel_unchecked(
                        src_base.add(src_offset),
                        src_pitch,
                        dst_base.add(dst_offset),
                        dst_pitch,
                        width,
                        height,
                    );
                } else {
                    converter.convert_rows_unchecked(
                        src_base.add(src_offset),
                        src_pitch,
                        dst_base.add(dst_offset),
                        dst_pitch,
                        width,
                        height,
                    );
                }
            }
        }

        Ok(converted)
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_trusted_direct_hints_vs_runtime_scan() {
        use std::hint::black_box;
        use std::time::Instant;

        const WIDTH: usize = 1920;
        const HEIGHT: usize = 1080;
        const ROUNDS: usize = 6;
        const ITERATIONS: usize = 2200;
        const MAX_REGRESSION: f64 = 5.0;

        let workloads = vec![
            (
                "ui_sparse_tiles_32",
                row_major_rects(24, 18, 8, 4, 32, 28, 24, 18, 32),
            ),
            (
                "chatty_small_updates_96",
                row_major_rects(8, 8, 16, 6, 10, 8, 8, 6, 96),
            ),
            (
                "dense_micro_tiles_180",
                row_major_rects(6, 6, 30, 6, 8, 6, 4, 3, 180),
            ),
        ];

        let src_pitch = WIDTH * 4;
        let dst_pitch = WIDTH * 4;
        let buffer_len = src_pitch * HEIGHT;
        let mut src = vec![0u8; buffer_len];
        for (idx, byte) in src.iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_mul(37).wrapping_add(11);
        }

        let converter = convert::SurfaceRowConverter::new(
            SurfacePixelFormat::Bgra8,
            SurfaceConversionOptions {
                hdr_to_sdr: None,
                ..SurfaceConversionOptions::default()
            },
        );

        let mut regressions = Vec::new();
        println!(
            "trusted-dirty benchmark: rounds={} iterations={} max_regression={:.2}%",
            ROUNDS, ITERATIONS, MAX_REGRESSION
        );
        println!(
            "{:<28} {:>12} {:>12} {:>9} {:>11}",
            "workload", "scan_ms", "hinted_ms", "speedup", "regression"
        );

        for (name, rects) in &workloads {
            let (non_empty_rects, dirty_pixels) = dirty_rect_stats(rects);
            let scan_hints = DirtyRectConversionHints::default();
            let hinted_hints = DirtyRectConversionHints {
                trusted_bounds: true,
                non_empty_rects: Some(non_empty_rects),
                total_dirty_pixels: Some(dirty_pixels),
            };

            let mut scan_out = vec![0u8; buffer_len];
            let mut hinted_out = vec![0u8; buffer_len];
            let converted_scan = unsafe {
                convert_dirty_rects_trusted_direct_unchecked(
                    SurfacePixelFormat::Bgra8,
                    converter,
                    src.as_ptr(),
                    src_pitch,
                    4,
                    scan_out.as_mut_ptr(),
                    dst_pitch,
                    rects,
                    0,
                    0,
                    scan_hints,
                    false,
                )
            }
            .expect("scan conversion failed");
            let converted_hinted = unsafe {
                convert_dirty_rects_trusted_direct_unchecked(
                    SurfacePixelFormat::Bgra8,
                    converter,
                    src.as_ptr(),
                    src_pitch,
                    4,
                    hinted_out.as_mut_ptr(),
                    dst_pitch,
                    rects,
                    0,
                    0,
                    hinted_hints,
                    false,
                )
            }
            .expect("hinted conversion failed");
            assert_eq!(
                converted_scan, converted_hinted,
                "converted rect count mismatch for workload {name}",
            );
            assert_eq!(
                scan_out, hinted_out,
                "scan/hinted output mismatch for workload {name}",
            );

            let mut best_scan = std::time::Duration::MAX;
            let mut best_hinted = std::time::Duration::MAX;

            for _round in 0..ROUNDS {
                let mut scan_checksum = 0u64;
                let mut scan_dst = vec![0u8; buffer_len];
                let scan_start = Instant::now();
                for iter in 0..ITERATIONS {
                    unsafe {
                        let converted = convert_dirty_rects_trusted_direct_unchecked(
                            SurfacePixelFormat::Bgra8,
                            converter,
                            src.as_ptr(),
                            src_pitch,
                            4,
                            scan_dst.as_mut_ptr(),
                            dst_pitch,
                            rects,
                            0,
                            0,
                            scan_hints,
                            false,
                        )
                        .expect("scan benchmark conversion failed");
                        scan_checksum = scan_checksum.wrapping_add(
                            scan_dst[(iter * 131 + converted) % scan_dst.len()] as u64,
                        );
                    }
                }
                black_box(scan_checksum);
                best_scan = best_scan.min(scan_start.elapsed());

                let mut hinted_checksum = 0u64;
                let mut hinted_dst = vec![0u8; buffer_len];
                let hinted_start = Instant::now();
                for iter in 0..ITERATIONS {
                    unsafe {
                        let converted = convert_dirty_rects_trusted_direct_unchecked(
                            SurfacePixelFormat::Bgra8,
                            converter,
                            src.as_ptr(),
                            src_pitch,
                            4,
                            hinted_dst.as_mut_ptr(),
                            dst_pitch,
                            rects,
                            0,
                            0,
                            hinted_hints,
                            false,
                        )
                        .expect("hinted benchmark conversion failed");
                        hinted_checksum = hinted_checksum.wrapping_add(
                            hinted_dst[(iter * 97 + converted) % hinted_dst.len()] as u64,
                        );
                    }
                }
                black_box(hinted_checksum);
                best_hinted = best_hinted.min(hinted_start.elapsed());
            }

            let scan_ms = best_scan.as_secs_f64() * 1000.0;
            let hinted_ms = best_hinted.as_secs_f64() * 1000.0;
            let speedup = if hinted_ms > 0.0 {
                scan_ms / hinted_ms
            } else {
                f64::INFINITY
            };
            let delta_pct = if scan_ms > 0.0 {
                ((hinted_ms - scan_ms) / scan_ms) * 100.0
            } else {
                0.0
            };
            let regression = if delta_pct > MAX_REGRESSION {
                "FAIL"
            } else if delta_pct > 0.0 {
                "warn"
            } else {
                "ok"
            };
            println!(
                "{:<28} {:>12.3} {:>12.3} {:>8.2}x {:>11}",
                name, scan_ms, hinted_ms, speedup, regression
            );
            if delta_pct > MAX_REGRESSION {
                regressions.push(format!(
                    "{} regressed by {:.2}% (scan {:.3} ms, hinted {:.3} ms)",
                    name, delta_pct, scan_ms, hinted_ms
                ));
            }
        }

        assert!(
            regressions.is_empty(),
            "hinted trusted-direct path regressed:\n{}",
            regressions.join("\n")
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_trusted_direct_bgra_batch_kernel_vs_baseline_dispatch() {
        use std::hint::black_box;
        use std::time::Instant;

        const WIDTH: usize = 1920;
        const HEIGHT: usize = 1080;
        const ROUNDS: usize = 6;
        const ITERATIONS: usize = 2200;
        const MAX_REGRESSION: f64 = 5.0;

        let workloads = vec![
            (
                "ui_sparse_tiles_32",
                row_major_rects(24, 18, 8, 4, 32, 28, 24, 18, 32),
            ),
            (
                "chatty_small_updates_96",
                row_major_rects(8, 8, 16, 6, 10, 8, 8, 6, 96),
            ),
            (
                "wide_strip_updates_20",
                row_major_rects(32, 24, 2, 10, 640, 18, 96, 10, 20),
            ),
            (
                "dense_micro_tiles_180",
                row_major_rects(6, 6, 30, 6, 8, 6, 4, 3, 180),
            ),
        ];

        let src_pitch = WIDTH * 4;
        let dst_pitch = WIDTH * 4;
        let buffer_len = src_pitch * HEIGHT;
        let mut src = vec![0u8; buffer_len];
        for (idx, byte) in src.iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_mul(37).wrapping_add(11);
        }

        let converter = convert::SurfaceRowConverter::new(
            SurfacePixelFormat::Bgra8,
            SurfaceConversionOptions {
                hdr_to_sdr: None,
                ..SurfaceConversionOptions::default()
            },
        );

        let mut regressions = Vec::new();
        println!(
            "trusted-dirty BGRA batch benchmark: rounds={} iterations={} max_regression={:.2}%",
            ROUNDS, ITERATIONS, MAX_REGRESSION
        );
        println!(
            "{:<28} {:>12} {:>12} {:>9} {:>11}",
            "workload", "baseline_ms", "batch_ms", "speedup", "regression"
        );

        for (name, rects) in &workloads {
            let (non_empty_rects, dirty_pixels) = dirty_rect_stats(rects);
            let hints = DirtyRectConversionHints {
                trusted_bounds: true,
                non_empty_rects: Some(non_empty_rects),
                total_dirty_pixels: Some(dirty_pixels),
            };

            let mut baseline_out = vec![0u8; buffer_len];
            let mut optimized_out = vec![0u8; buffer_len];
            let baseline_converted = unsafe {
                convert_dirty_rects_trusted_direct_baseline_unchecked(
                    converter,
                    src.as_ptr(),
                    src_pitch,
                    4,
                    baseline_out.as_mut_ptr(),
                    dst_pitch,
                    rects,
                    0,
                    0,
                    hints,
                )
            }
            .expect("baseline conversion failed");
            let optimized_converted = unsafe {
                convert_dirty_rects_trusted_direct_unchecked(
                    SurfacePixelFormat::Bgra8,
                    converter,
                    src.as_ptr(),
                    src_pitch,
                    4,
                    optimized_out.as_mut_ptr(),
                    dst_pitch,
                    rects,
                    0,
                    0,
                    hints,
                    false,
                )
            }
            .expect("optimized conversion failed");
            assert_eq!(
                baseline_converted, optimized_converted,
                "converted rect count mismatch for workload {name}",
            );
            assert_eq!(
                baseline_out, optimized_out,
                "baseline/optimized output mismatch for workload {name}",
            );

            let mut best_baseline = std::time::Duration::MAX;
            let mut best_optimized = std::time::Duration::MAX;

            for _round in 0..ROUNDS {
                let mut baseline_checksum = 0u64;
                let mut baseline_dst = vec![0u8; buffer_len];
                let baseline_start = Instant::now();
                for iter in 0..ITERATIONS {
                    unsafe {
                        let converted = convert_dirty_rects_trusted_direct_baseline_unchecked(
                            converter,
                            src.as_ptr(),
                            src_pitch,
                            4,
                            baseline_dst.as_mut_ptr(),
                            dst_pitch,
                            rects,
                            0,
                            0,
                            hints,
                        )
                        .expect("baseline benchmark conversion failed");
                        baseline_checksum = baseline_checksum.wrapping_add(
                            baseline_dst[(iter * 131 + converted) % baseline_dst.len()] as u64,
                        );
                    }
                }
                black_box(baseline_checksum);
                best_baseline = best_baseline.min(baseline_start.elapsed());

                let mut optimized_checksum = 0u64;
                let mut optimized_dst = vec![0u8; buffer_len];
                let optimized_start = Instant::now();
                for iter in 0..ITERATIONS {
                    unsafe {
                        let converted = convert_dirty_rects_trusted_direct_unchecked(
                            SurfacePixelFormat::Bgra8,
                            converter,
                            src.as_ptr(),
                            src_pitch,
                            4,
                            optimized_dst.as_mut_ptr(),
                            dst_pitch,
                            rects,
                            0,
                            0,
                            hints,
                            false,
                        )
                        .expect("optimized benchmark conversion failed");
                        optimized_checksum = optimized_checksum.wrapping_add(
                            optimized_dst[(iter * 97 + converted) % optimized_dst.len()] as u64,
                        );
                    }
                }
                black_box(optimized_checksum);
                best_optimized = best_optimized.min(optimized_start.elapsed());
            }

            let baseline_ms = best_baseline.as_secs_f64() * 1000.0;
            let optimized_ms = best_optimized.as_secs_f64() * 1000.0;
            let speedup = if optimized_ms > 0.0 {
                baseline_ms / optimized_ms
            } else {
                f64::INFINITY
            };
            let delta_pct = if baseline_ms > 0.0 {
                ((optimized_ms - baseline_ms) / baseline_ms) * 100.0
            } else {
                0.0
            };
            let regression = if delta_pct > MAX_REGRESSION {
                "FAIL"
            } else if delta_pct > 0.0 {
                "warn"
            } else {
                "ok"
            };
            println!(
                "{:<28} {:>12.3} {:>12.3} {:>8.2}x {:>11}",
                name, baseline_ms, optimized_ms, speedup, regression
            );
            if delta_pct > MAX_REGRESSION {
                regressions.push(format!(
                    "{} regressed by {:.2}% (baseline {:.3} ms, optimized {:.3} ms)",
                    name, delta_pct, baseline_ms, optimized_ms
                ));
            }
        }

        assert!(
            regressions.is_empty(),
            "BGRA dirty-rect batch kernel regressed:\n{}",
            regressions.join("\n")
        );
    }
}

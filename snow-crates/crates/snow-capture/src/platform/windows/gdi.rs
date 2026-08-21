use std::ffi::c_void;
use std::mem::size_of;
use std::ptr::null_mut;
use std::sync::Arc;
use std::time::Instant;

use anyhow::Context;
use windows::Win32::Foundation::{HANDLE, HWND, POINT, RECT};
use windows::Win32::Graphics::Gdi::{
    BI_RGB, BITMAPINFO, BITMAPINFOHEADER, BitBlt, CreateCompatibleDC, CreateDCW, CreateDIBSection,
    DIB_RGB_COLORS, DeleteDC, DeleteObject, GetDC, GetMonitorInfoW, GetWindowDC, HBITMAP, HDC,
    HGDIOBJ, HMONITOR, MONITOR_DEFAULTTOPRIMARY, MONITORINFO, MONITORINFOEXW, MonitorFromPoint,
    ReleaseDC, SRCCOPY, SelectObject,
};
use windows::Win32::Storage::Xps::{PRINT_WINDOW_FLAGS, PrintWindow};
use windows::Win32::UI::WindowsAndMessaging::{GetWindowRect, IsIconic, IsWindow, IsWindowVisible};
use windows::core::{PCWSTR, w};

use crate::backend::{CaptureBackendKind, CaptureBlitRegion, CaptureMode, CaptureSampleMetadata};
use crate::capture_session::CaptureTargetInfo;
use crate::convert;
use crate::error::{CaptureError, CaptureResult};
use crate::frame::Frame;
use crate::monitor::MonitorId;

use super::monitor::MonitorResolver;

#[derive(Clone, Copy, PartialEq, Eq)]
struct MonitorGeometry {
    handle: HMONITOR,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
}

fn geometry_from_handle(handle: HMONITOR) -> CaptureResult<MonitorGeometry> {
    let mut info = MONITORINFOEXW {
        monitorInfo: MONITORINFO {
            cbSize: size_of::<MONITORINFOEXW>() as u32,
            ..Default::default()
        },
        ..Default::default()
    };

    if !unsafe { GetMonitorInfoW(handle, (&mut info as *mut MONITORINFOEXW).cast()) }.as_bool() {
        return Err(CaptureError::MonitorLost);
    }

    let rect = info.monitorInfo.rcMonitor;
    let width = rect.right - rect.left;
    let height = rect.bottom - rect.top;
    if width <= 0 || height <= 0 {
        return Err(CaptureError::platform(anyhow::anyhow!(
            "monitor geometry is invalid ({width}x{height})"
        )));
    }

    Ok(MonitorGeometry {
        handle,
        left: rect.left,
        top: rect.top,
        width,
        height,
    })
}

#[derive(Clone, PartialEq, Eq)]
struct PrimaryMonitorBinding {
    geometry: MonitorGeometry,
    device_name: [u16; 32],
}

fn query_primary_monitor_binding() -> CaptureResult<PrimaryMonitorBinding> {
    let handle = unsafe { MonitorFromPoint(POINT { x: 0, y: 0 }, MONITOR_DEFAULTTOPRIMARY) };
    if handle.0.is_null() {
        return Err(CaptureError::NoPrimaryMonitor);
    }

    let mut info = MONITORINFOEXW {
        monitorInfo: MONITORINFO {
            cbSize: size_of::<MONITORINFOEXW>() as u32,
            ..Default::default()
        },
        ..Default::default()
    };
    if !unsafe { GetMonitorInfoW(handle, (&mut info as *mut MONITORINFOEXW).cast()) }.as_bool() {
        return Err(CaptureError::MonitorLost);
    }

    let rect = info.monitorInfo.rcMonitor;
    let width = rect.right - rect.left;
    let height = rect.bottom - rect.top;
    if width <= 0 || height <= 0 {
        return Err(CaptureError::platform(anyhow::anyhow!(
            "primary monitor geometry is invalid ({width}x{height})"
        )));
    }

    Ok(PrimaryMonitorBinding {
        geometry: MonitorGeometry {
            handle,
            left: rect.left,
            top: rect.top,
            width,
            height,
        },
        device_name: info.szDevice,
    })
}

pub(crate) fn inspect_primary_monitor() -> CaptureResult<CaptureTargetInfo> {
    let binding = query_primary_monitor_binding()?;
    Ok(CaptureTargetInfo {
        origin_x: binding.geometry.left,
        origin_y: binding.geometry.top,
        width: binding.geometry.width as u32,
        height: binding.geometry.height as u32,
    })
}

fn resolve_geometry(resolver: &MonitorResolver, id: &MonitorId) -> CaptureResult<MonitorGeometry> {
    let resolved = resolver.resolve_monitor(id)?;
    geometry_from_handle(resolved.handle)
}

fn create_monitor_screen_dc(handle: HMONITOR) -> CaptureResult<HDC> {
    let mut info = MONITORINFOEXW {
        monitorInfo: MONITORINFO {
            cbSize: size_of::<MONITORINFOEXW>() as u32,
            ..Default::default()
        },
        ..Default::default()
    };

    if !unsafe { GetMonitorInfoW(handle, (&mut info as *mut MONITORINFOEXW).cast()) }.as_bool() {
        return Err(CaptureError::MonitorLost);
    }

    create_monitor_screen_dc_for_device(&info.szDevice)
}

fn create_monitor_screen_dc_for_device(device_name: &[u16]) -> CaptureResult<HDC> {
    let dc = unsafe {
        CreateDCW(
            w!("DISPLAY"),
            PCWSTR(device_name.as_ptr()),
            PCWSTR::null(),
            None,
        )
    };
    if dc.0.is_null() {
        return Err(CaptureError::platform(anyhow::anyhow!(
            "CreateDCW failed for monitor device"
        )));
    }
    Ok(dc)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum WindowCapturePath {
    WindowDcBitBlt,
    PrintWindow(PRINT_WINDOW_FLAGS),
}

const PRINT_WINDOW_RENDER_FULL: PRINT_WINDOW_FLAGS = PRINT_WINDOW_FLAGS(2);
const PRINT_WINDOW_DEFAULT: PRINT_WINDOW_FLAGS = PRINT_WINDOW_FLAGS(0);
const PRINT_WINDOW_EXPERIMENTAL: PRINT_WINDOW_FLAGS = PRINT_WINDOW_FLAGS(4);

const WINDOW_CAPTURE_ORDER_SCREENSHOT: [WindowCapturePath; 4] = [
    WindowCapturePath::WindowDcBitBlt,
    WindowCapturePath::PrintWindow(PRINT_WINDOW_RENDER_FULL),
    WindowCapturePath::PrintWindow(PRINT_WINDOW_DEFAULT),
    WindowCapturePath::PrintWindow(PRINT_WINDOW_EXPERIMENTAL),
];

const WINDOW_CAPTURE_ORDER_RECORDING: [WindowCapturePath; 4] = [
    WindowCapturePath::WindowDcBitBlt,
    WindowCapturePath::PrintWindow(PRINT_WINDOW_RENDER_FULL),
    WindowCapturePath::PrintWindow(PRINT_WINDOW_DEFAULT),
    WindowCapturePath::PrintWindow(PRINT_WINDOW_EXPERIMENTAL),
];

#[inline]
fn gdi_window_state_refresh_interval_frames() -> u32 {
    8
}

const GDI_SCREENSHOT_NT_MIN_PIXELS: usize = 1280 * 720;

#[inline(always)]
fn use_gdi_nt_bgra_conversion(
    mode: CaptureMode,
    destination_has_history: bool,
    pixel_count: usize,
) -> bool {
    match mode {
        CaptureMode::Continuous => true,
        CaptureMode::Snapshot => {
            destination_has_history || pixel_count >= GDI_SCREENSHOT_NT_MIN_PIXELS
        }
    }
}

#[inline(always)]
unsafe fn convert_gdi_bgra_surface_to_rgba(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    mode: CaptureMode,
    destination_has_history: bool,
) {
    if mode == CaptureMode::Snapshot {
        if use_gdi_nt_bgra_conversion(mode, destination_has_history, pixel_count) {
            unsafe {
                convert::convert_bgra_to_rgba_opaque_nt_serial_unchecked(src, dst, pixel_count);
            }
        } else {
            unsafe {
                convert::convert_bgra_to_rgba_opaque_serial_unchecked(src, dst, pixel_count);
            }
        }
        return;
    }

    if use_gdi_nt_bgra_conversion(mode, destination_has_history, pixel_count) {
        unsafe {
            convert::convert_bgra_to_rgba_opaque_nt_unchecked(src, dst, pixel_count);
        }
    } else {
        unsafe {
            convert::convert_bgra_to_rgba_opaque_unchecked(src, dst, pixel_count);
        }
    }
}

const GDI_INCREMENTAL_MIN_PIXELS: usize = 160_000;
const GDI_INCREMENTAL_SPAN_MIN_PIXELS: usize = 131_072;
const GDI_INCREMENTAL_DUPLICATE_PROBE_MIN_PIXELS: usize = 262_144;
const GDI_INCREMENTAL_MAX_DIRTY_ROW_NUMERATOR: usize = 3;
const GDI_INCREMENTAL_MAX_DIRTY_ROW_DENOMINATOR: usize = 4;
const GDI_INCREMENTAL_TOO_DIRTY_PROBE_MIN_PIXELS: usize = 786_432;
const GDI_INCREMENTAL_TOO_DIRTY_PROBE_MAX_ROWS: usize = 24;
const GDI_INCREMENTAL_TOO_DIRTY_PROBE_DIRTY_NUMERATOR: usize = 4;
const GDI_INCREMENTAL_TOO_DIRTY_PROBE_DIRTY_DENOMINATOR: usize = 5;
const GDI_PARALLEL_ROW_SCAN_MIN_PIXELS: usize = 2_000_000;
const GDI_PARALLEL_ROW_SCAN_MIN_CHUNK_PIXELS: usize = 131_072;
const GDI_PARALLEL_ROW_SCAN_MAX_WORKERS: usize = usize::MAX;
const GDI_PARALLEL_SPAN_SINGLE_SCAN_PROBE_ROWS: usize = 8;
const GDI_PARALLEL_SPAN_HINT_FORCE_SINGLE_ROW_NUMERATOR: usize = 7;
const GDI_PARALLEL_SPAN_HINT_FORCE_SINGLE_ROW_DENOMINATOR: usize = 8;
const GDI_PARALLEL_SPAN_HINT_DENSE_ROW_NUMERATOR: usize = 2;
const GDI_PARALLEL_SPAN_HINT_DENSE_ROW_DENOMINATOR: usize = 3;
const GDI_PARALLEL_SPAN_HINT_DENSE_PIXEL_NUMERATOR: usize = 3;
const GDI_PARALLEL_SPAN_HINT_DENSE_PIXEL_DENOMINATOR: usize = 10;
const BGRA_BYTES_PER_PIXEL: usize = 4;

#[derive(Clone, Copy, Debug)]
struct DirtyRowRun {
    start_row: usize,
    row_count: usize,
}

#[derive(Clone, Copy, Debug, Default)]
struct DirtyRowSpan {
    start_col: u32,
    width: u32,
}

#[derive(Clone, Copy, Debug)]
struct DirtySpanRun {
    start_row: usize,
    row_count: usize,
    start_col: usize,
    width: usize,
}

#[derive(Clone, Copy, Debug)]
struct DirtyScanResult {
    dirty_rows: usize,
    used_spans: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ParallelSpanScanMode {
    CompareThenDiff,
    SingleScanDiff,
}

fn build_dirty_row_runs_from_flags(flags: &[u8], runs: &mut Vec<DirtyRowRun>) {
    runs.clear();

    let mut run_start = None::<usize>;
    for (row, &flag) in flags.iter().enumerate() {
        if flag == 0 {
            if let Some(start) = run_start.take() {
                runs.push(DirtyRowRun {
                    start_row: start,
                    row_count: row - start,
                });
            }
            continue;
        }

        if run_start.is_none() {
            run_start = Some(row);
        }
    }

    if let Some(start) = run_start {
        runs.push(DirtyRowRun {
            start_row: start,
            row_count: flags.len() - start,
        });
    }
}

fn build_dirty_span_runs_from_spans(spans: &[DirtyRowSpan], runs: &mut Vec<DirtySpanRun>) {
    runs.clear();
    let mut active_run: Option<DirtySpanRun> = None;

    for (row, span) in spans.iter().enumerate() {
        if span.width == 0 {
            if let Some(run) = active_run.take() {
                runs.push(run);
            }
            continue;
        }

        let start_col = span.start_col as usize;
        let width = span.width as usize;
        if let Some(run) = active_run.as_mut()
            && run.start_row + run.row_count == row
            && run.start_col == start_col
            && run.width == width
        {
            run.row_count += 1;
            continue;
        }

        if let Some(run) = active_run.take() {
            runs.push(run);
        }
        active_run = Some(DirtySpanRun {
            start_row: row,
            row_count: 1,
            start_col,
            width,
        });
    }

    if let Some(run) = active_run {
        runs.push(run);
    }
}

fn scan_dirty_row_flags_parallel(
    flags: &mut [u8],
    src_base: *const u8,
    src_stride: usize,
    history_base: *mut u8,
    history_stride: usize,
    row_bytes: usize,
    compare_row: RowCompareKernel,
    copy_changed_rows_to_history: bool,
) -> usize {
    let src_addr = src_base as usize;
    let history_addr = history_base as usize;
    let mut dirty_rows = 0usize;
    convert::with_conversion_pool(GDI_PARALLEL_ROW_SCAN_MAX_WORKERS, || {
        use rayon::prelude::*;
        dirty_rows = flags
            .par_iter_mut()
            .enumerate()
            .map(|(row, flag)| {
                let src_row = (src_addr + row * src_stride) as *const u8;
                let history_row = (history_addr + row * history_stride) as *mut u8;
                let changed = unsafe { !compare_row(src_row, history_row.cast_const(), row_bytes) };
                *flag = if changed { 1u8 } else { 0u8 };
                if !changed {
                    return 0usize;
                }

                if copy_changed_rows_to_history {
                    unsafe {
                        std::ptr::copy_nonoverlapping(src_row, history_row, row_bytes);
                    }
                }
                1usize
            })
            .sum::<usize>();
    });
    dirty_rows
}

fn scan_dirty_row_spans_parallel(
    spans: &mut [DirtyRowSpan],
    src_base: *const u8,
    src_stride: usize,
    history_base: *mut u8,
    history_stride: usize,
    row_bytes: usize,
    width: usize,
    compare_row: RowCompareKernel,
    row_diff_bounds: RowDiffBoundsKernel,
    copy_changed_rows_to_history: bool,
    scan_mode: ParallelSpanScanMode,
) -> (usize, usize) {
    debug_assert!(width <= u32::MAX as usize);
    let src_addr = src_base as usize;
    let history_addr = history_base as usize;
    let mut totals = (0usize, 0usize);
    convert::with_conversion_pool(GDI_PARALLEL_ROW_SCAN_MAX_WORKERS, || {
        use rayon::prelude::*;
        totals = spans
            .par_iter_mut()
            .enumerate()
            .map(|(row, span)| {
                let src_row = (src_addr + row * src_stride) as *const u8;
                let history_row = (history_addr + row * history_stride) as *mut u8;

                let dirty_span = match scan_mode {
                    ParallelSpanScanMode::CompareThenDiff => {
                        let changed =
                            unsafe { !compare_row(src_row, history_row.cast_const(), row_bytes) };
                        if !changed {
                            None
                        } else {
                            unsafe {
                                row_diff_span_pixels_with_kernel(
                                    row_diff_bounds,
                                    src_row,
                                    history_row.cast_const(),
                                    row_bytes,
                                    width,
                                )
                            }
                            .or(Some((0usize, width)))
                        }
                    }
                    ParallelSpanScanMode::SingleScanDiff => unsafe {
                        row_diff_span_pixels_with_kernel(
                            row_diff_bounds,
                            src_row,
                            history_row.cast_const(),
                            row_bytes,
                            width,
                        )
                    },
                };

                let Some((start_col, end_col)) = dirty_span else {
                    *span = DirtyRowSpan::default();
                    return (0usize, 0usize);
                };

                let span_width = end_col.saturating_sub(start_col);
                *span = DirtyRowSpan {
                    start_col: start_col as u32,
                    width: span_width as u32,
                };

                if copy_changed_rows_to_history {
                    let span_start_bytes = start_col * BGRA_BYTES_PER_PIXEL;
                    let copy_bytes = span_width * BGRA_BYTES_PER_PIXEL;
                    if copy_bytes > 0 {
                        unsafe {
                            std::ptr::copy_nonoverlapping(
                                src_row.add(span_start_bytes),
                                history_row.add(span_start_bytes),
                                copy_bytes,
                            );
                        }
                    }
                }

                (1usize, span_width)
            })
            .reduce(
                || (0usize, 0usize),
                |lhs, rhs| (lhs.0.saturating_add(rhs.0), lhs.1.saturating_add(rhs.1)),
            );
    });

    totals
}

unsafe fn row_diff_bounds_scalar(
    lhs: *const u8,
    rhs: *const u8,
    len: usize,
) -> Option<(usize, usize)> {
    if len == 0 {
        return None;
    }

    let mut start = 0usize;
    while start + size_of::<u64>() <= len {
        let left = unsafe { std::ptr::read_unaligned(lhs.add(start).cast::<u64>()) };
        let right = unsafe { std::ptr::read_unaligned(rhs.add(start).cast::<u64>()) };
        if left != right {
            break;
        }
        start += size_of::<u64>();
    }
    while start < len && unsafe { *lhs.add(start) == *rhs.add(start) } {
        start += 1;
    }
    if start == len {
        return None;
    }

    let mut end = len;
    while end >= size_of::<u64>() {
        let block_start = end - size_of::<u64>();
        let left = unsafe { std::ptr::read_unaligned(lhs.add(block_start).cast::<u64>()) };
        let right = unsafe { std::ptr::read_unaligned(rhs.add(block_start).cast::<u64>()) };
        if left != right {
            break;
        }
        end -= size_of::<u64>();
    }
    while end > start && unsafe { *lhs.add(end - 1) == *rhs.add(end - 1) } {
        end -= 1;
    }

    Some((start, end))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum IncrementalConvertStatus {
    /// No usable history available yet (first frame or surface resize).
    NotAvailable,
    /// Incremental scan completed and found no changed rows.
    Duplicate,
    /// Incremental conversion completed with `dirty_rows` updated rows.
    Updated(usize),
    /// Incremental scan exceeded the dirty threshold; caller should fall
    /// back to a full-frame conversion.
    TooDirty,
}

type RowCompareKernel = unsafe fn(*const u8, *const u8, usize) -> bool;
type RowDiffBoundsKernel = unsafe fn(*const u8, *const u8, usize) -> Option<(usize, usize)>;

#[inline(always)]
fn incremental_too_dirty_probe_eligible(pixel_count: usize, height: usize) -> bool {
    pixel_count >= GDI_INCREMENTAL_TOO_DIRTY_PROBE_MIN_PIXELS && height > 1
}

#[inline(always)]
fn incremental_duplicate_probe_eligible(
    pixel_count: usize,
    row_bytes: usize,
    src_stride: usize,
    history_stride: usize,
) -> bool {
    pixel_count >= GDI_INCREMENTAL_DUPLICATE_PROBE_MIN_PIXELS
        && row_bytes > 0
        && src_stride == row_bytes
        && history_stride == row_bytes
}

#[inline(always)]
fn parallel_row_scan_eligible(pixel_count: usize, height: usize) -> bool {
    height > 1
        && convert::should_parallelize_work(
            pixel_count,
            GDI_PARALLEL_ROW_SCAN_MIN_PIXELS,
            GDI_PARALLEL_ROW_SCAN_MIN_CHUNK_PIXELS,
            GDI_PARALLEL_ROW_SCAN_MAX_WORKERS,
        )
}

#[inline(always)]
fn incremental_span_convert_eligible(pixel_count: usize, width: usize, height: usize) -> bool {
    width > 1 && height > 1 && pixel_count >= GDI_INCREMENTAL_SPAN_MIN_PIXELS
}

#[inline(always)]
fn parallel_span_scan_eligible(
    pixel_count: usize,
    width: usize,
    height: usize,
    incremental_too_dirty_hint: bool,
) -> bool {
    !incremental_too_dirty_hint
        && parallel_row_scan_eligible(pixel_count, height)
        && incremental_span_convert_eligible(pixel_count, width, height)
}

unsafe fn parallel_span_single_scan_probe_detects_change(
    src_base: *const u8,
    src_stride: usize,
    history_base: *const u8,
    history_stride: usize,
    row_bytes: usize,
    height: usize,
    compare_row: RowCompareKernel,
) -> bool {
    if height == 0 || row_bytes == 0 {
        return false;
    }

    let sample_rows = height.clamp(1, GDI_PARALLEL_SPAN_SINGLE_SCAN_PROBE_ROWS);
    if sample_rows == 1 {
        return unsafe { !compare_row(src_base, history_base, row_bytes) };
    }

    let denom = sample_rows - 1;
    for sample_idx in 0..sample_rows {
        let row = sample_idx.saturating_mul(height - 1) / denom;
        let src_row = unsafe { src_base.add(row * src_stride) };
        let history_row = unsafe { history_base.add(row * history_stride) };
        if unsafe { !compare_row(src_row, history_row, row_bytes) } {
            return true;
        }
    }
    false
}

#[inline(always)]
fn recommend_parallel_span_scan_mode(
    dirty_rows: usize,
    dirty_pixels: usize,
    width: usize,
    height: usize,
) -> ParallelSpanScanMode {
    if dirty_rows == 0 || width == 0 || height == 0 {
        return ParallelSpanScanMode::CompareThenDiff;
    }

    let total_pixels = width.saturating_mul(height).max(1);
    let force_single_scan = dirty_rows
        .saturating_mul(GDI_PARALLEL_SPAN_HINT_FORCE_SINGLE_ROW_DENOMINATOR)
        >= height.saturating_mul(GDI_PARALLEL_SPAN_HINT_FORCE_SINGLE_ROW_NUMERATOR);
    if force_single_scan {
        return ParallelSpanScanMode::SingleScanDiff;
    }

    let dense_rows = dirty_rows.saturating_mul(GDI_PARALLEL_SPAN_HINT_DENSE_ROW_DENOMINATOR)
        >= height.saturating_mul(GDI_PARALLEL_SPAN_HINT_DENSE_ROW_NUMERATOR);
    let dense_pixels = dirty_pixels.saturating_mul(GDI_PARALLEL_SPAN_HINT_DENSE_PIXEL_DENOMINATOR)
        >= total_pixels.saturating_mul(GDI_PARALLEL_SPAN_HINT_DENSE_PIXEL_NUMERATOR);

    if dense_rows && dense_pixels {
        ParallelSpanScanMode::SingleScanDiff
    } else {
        ParallelSpanScanMode::CompareThenDiff
    }
}

#[inline(always)]
fn select_parallel_span_scan_mode(
    src_base: *const u8,
    src_stride: usize,
    history_base: *const u8,
    history_stride: usize,
    row_bytes: usize,
    height: usize,
    compare_row: RowCompareKernel,
    duplicate_probe_hint: bool,
    mode_hint: Option<ParallelSpanScanMode>,
) -> ParallelSpanScanMode {
    if duplicate_probe_hint {
        return ParallelSpanScanMode::CompareThenDiff;
    }
    if let Some(mode) = mode_hint {
        return mode;
    }
    if unsafe {
        parallel_span_single_scan_probe_detects_change(
            src_base,
            src_stride,
            history_base,
            history_stride,
            row_bytes,
            height,
            compare_row,
        )
    } {
        ParallelSpanScanMode::SingleScanDiff
    } else {
        ParallelSpanScanMode::CompareThenDiff
    }
}

unsafe fn incremental_too_dirty_probe(
    src_base: *const u8,
    src_stride: usize,
    history_base: *const u8,
    history_stride: usize,
    row_bytes: usize,
    height: usize,
    compare_row: RowCompareKernel,
) -> bool {
    if height == 0 || row_bytes == 0 {
        return false;
    }

    let sample_rows = height.clamp(1, GDI_INCREMENTAL_TOO_DIRTY_PROBE_MAX_ROWS);
    let dirty_threshold_lhs =
        sample_rows.saturating_mul(GDI_INCREMENTAL_TOO_DIRTY_PROBE_DIRTY_NUMERATOR);

    let mut dirty_samples = 0usize;
    for sample_idx in 0..sample_rows {
        // Evenly sample rows across the surface instead of scanning from
        // the top only, so localised animations do not trigger false
        // "too dirty" classifications.
        let row = sample_idx
            .checked_mul(height)
            .map(|value| value / sample_rows)
            .unwrap_or(height - 1)
            .min(height - 1);
        let Some(src_offset) = row.checked_mul(src_stride) else {
            return true;
        };
        let Some(history_offset) = row.checked_mul(history_stride) else {
            return true;
        };

        let src_row = unsafe { src_base.add(src_offset) };
        let history_row = unsafe { history_base.add(history_offset) };
        let changed = unsafe { !compare_row(src_row, history_row, row_bytes) };
        if changed {
            dirty_samples = dirty_samples.saturating_add(1);
            let dirty_lhs =
                dirty_samples.saturating_mul(GDI_INCREMENTAL_TOO_DIRTY_PROBE_DIRTY_DENOMINATOR);
            if dirty_lhs > dirty_threshold_lhs {
                return true;
            }
        }
    }

    false
}

#[inline(always)]
fn row_compare_kernel() -> RowCompareKernel {
    use std::sync::OnceLock;
    static KERNEL: OnceLock<RowCompareKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_row_compare_kernel)
}

fn select_row_compare_kernel() -> RowCompareKernel {
    #[cfg(target_arch = "x86_64")]
    {
        let use_unrolled = true;
        let use_bidirectional = true;
        if std::arch::is_x86_feature_detected!("avx2") {
            return if use_unrolled {
                if use_bidirectional {
                    row_equal_avx2_unrolled
                } else {
                    row_equal_avx2_unrolled_baseline
                }
            } else {
                row_equal_avx2_baseline
            };
        }
        if std::arch::is_x86_feature_detected!("sse2") {
            return if use_unrolled {
                if use_bidirectional {
                    row_equal_sse2_unrolled
                } else {
                    row_equal_sse2_unrolled_baseline
                }
            } else {
                row_equal_sse2_baseline
            };
        }
    }
    row_equal_scalar
}

#[inline(always)]
fn row_diff_bounds_kernel() -> RowDiffBoundsKernel {
    use std::sync::OnceLock;
    static KERNEL: OnceLock<RowDiffBoundsKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_row_diff_bounds_kernel)
}

fn select_row_diff_bounds_kernel() -> RowDiffBoundsKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx2") {
            return row_diff_bounds_avx2;
        }
        if std::arch::is_x86_feature_detected!("sse2") {
            return row_diff_bounds_sse2;
        }
    }
    row_diff_bounds_scalar
}

#[inline(always)]
fn bgra_row_converter() -> convert::SurfaceRowConverter {
    use std::sync::OnceLock;
    static CONVERTER: OnceLock<convert::SurfaceRowConverter> = OnceLock::new();
    *CONVERTER.get_or_init(|| {
        convert::SurfaceRowConverter::new(
            convert::SurfacePixelFormat::Bgra8,
            convert::SurfaceConversionOptions::default(),
        )
    })
}

#[inline(always)]
unsafe fn row_diff_span_pixels(
    lhs: *const u8,
    rhs: *const u8,
    row_bytes: usize,
    width: usize,
) -> Option<(usize, usize)> {
    unsafe {
        row_diff_span_pixels_with_kernel(row_diff_bounds_kernel(), lhs, rhs, row_bytes, width)
    }
}

#[inline(always)]
unsafe fn row_diff_span_pixels_with_kernel(
    row_diff_bounds: RowDiffBoundsKernel,
    lhs: *const u8,
    rhs: *const u8,
    row_bytes: usize,
    width: usize,
) -> Option<(usize, usize)> {
    let (start_byte, end_byte) = unsafe { row_diff_bounds(lhs, rhs, row_bytes)? };
    let start_col = start_byte / BGRA_BYTES_PER_PIXEL;
    let end_col = end_byte
        .checked_add(BGRA_BYTES_PER_PIXEL - 1)
        .map(|value| value / BGRA_BYTES_PER_PIXEL)
        .unwrap_or(width)
        .min(width);
    if end_col <= start_col {
        return None;
    }
    Some((start_col, end_col))
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn row_diff_bounds_sse2(
    lhs: *const u8,
    rhs: *const u8,
    len: usize,
) -> Option<(usize, usize)> {
    use std::arch::x86_64::{__m128i, _mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8};

    if len == 0 {
        return None;
    }

    let mut start = 0usize;
    while start + 16 <= len {
        let left = unsafe { _mm_loadu_si128(lhs.add(start).cast::<__m128i>()) };
        let right = unsafe { _mm_loadu_si128(rhs.add(start).cast::<__m128i>()) };
        let eq_mask = _mm_movemask_epi8(_mm_cmpeq_epi8(left, right)) as u32;
        if eq_mask != 0xFFFF {
            let diff_mask = (!eq_mask) & 0xFFFF;
            start += diff_mask.trailing_zeros() as usize;
            break;
        }
        start += 16;
    }
    while start < len && unsafe { *lhs.add(start) == *rhs.add(start) } {
        start += 1;
    }
    if start == len {
        return None;
    }

    let mut end = len;
    while end > start {
        let remaining = end - start;
        if remaining < 16 {
            break;
        }
        let block_start = end - 16;
        let left = unsafe { _mm_loadu_si128(lhs.add(block_start).cast::<__m128i>()) };
        let right = unsafe { _mm_loadu_si128(rhs.add(block_start).cast::<__m128i>()) };
        let eq_mask = _mm_movemask_epi8(_mm_cmpeq_epi8(left, right)) as u32;
        if eq_mask != 0xFFFF {
            let diff_mask = (!eq_mask) & 0xFFFF;
            let highest = (31 - diff_mask.leading_zeros()) as usize;
            end = block_start + highest + 1;
            return Some((start, end));
        }
        end -= 16;
    }
    while end > start && unsafe { *lhs.add(end - 1) == *rhs.add(end - 1) } {
        end -= 1;
    }

    Some((start, end))
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn row_diff_bounds_avx2(
    lhs: *const u8,
    rhs: *const u8,
    len: usize,
) -> Option<(usize, usize)> {
    use std::arch::x86_64::{__m256i, _mm256_cmpeq_epi8, _mm256_loadu_si256, _mm256_movemask_epi8};

    if len == 0 {
        return None;
    }

    let mut start = 0usize;
    while start + 32 <= len {
        let left = unsafe { _mm256_loadu_si256(lhs.add(start).cast::<__m256i>()) };
        let right = unsafe { _mm256_loadu_si256(rhs.add(start).cast::<__m256i>()) };
        let eq_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(left, right)) as u32;
        if eq_mask != u32::MAX {
            let diff_mask = !eq_mask;
            start += diff_mask.trailing_zeros() as usize;
            break;
        }
        start += 32;
    }
    while start < len && unsafe { *lhs.add(start) == *rhs.add(start) } {
        start += 1;
    }
    if start == len {
        return None;
    }

    let mut end = len;
    while end > start {
        let remaining = end - start;
        if remaining < 32 {
            break;
        }
        let block_start = end - 32;
        let left = unsafe { _mm256_loadu_si256(lhs.add(block_start).cast::<__m256i>()) };
        let right = unsafe { _mm256_loadu_si256(rhs.add(block_start).cast::<__m256i>()) };
        let eq_mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(left, right)) as u32;
        if eq_mask != u32::MAX {
            let diff_mask = !eq_mask;
            let highest = (31 - diff_mask.leading_zeros()) as usize;
            end = block_start + highest + 1;
            return Some((start, end));
        }
        end -= 32;
    }
    while end > start && unsafe { *lhs.add(end - 1) == *rhs.add(end - 1) } {
        end -= 1;
    }

    Some((start, end))
}

unsafe fn row_equal_scalar(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    let mut offset = 0usize;
    while offset + size_of::<u64>() <= len {
        let left = unsafe { std::ptr::read_unaligned(lhs.add(offset).cast::<u64>()) };
        let right = unsafe { std::ptr::read_unaligned(rhs.add(offset).cast::<u64>()) };
        if left != right {
            return false;
        }
        offset += size_of::<u64>();
    }

    while offset < len {
        if unsafe { *lhs.add(offset) } != unsafe { *rhs.add(offset) } {
            return false;
        }
        offset += 1;
    }
    true
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn row_equal_sse2_baseline(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{__m128i, _mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8};

    let mut offset = 0usize;
    while offset + 16 <= len {
        let left = unsafe { _mm_loadu_si128(lhs.add(offset).cast::<__m128i>()) };
        let right = unsafe { _mm_loadu_si128(rhs.add(offset).cast::<__m128i>()) };
        let equals = _mm_cmpeq_epi8(left, right);
        if _mm_movemask_epi8(equals) != 0xFFFF_i32 {
            return false;
        }
        offset += 16;
    }

    unsafe { row_equal_scalar(lhs.add(offset), rhs.add(offset), len - offset) }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn row_equal_sse2_unrolled_baseline(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{
        __m128i, _mm_and_si128, _mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8,
    };

    let mut offset = 0usize;
    while offset + 64 <= len {
        let left0 = unsafe { _mm_loadu_si128(lhs.add(offset).cast::<__m128i>()) };
        let right0 = unsafe { _mm_loadu_si128(rhs.add(offset).cast::<__m128i>()) };
        let eq0 = _mm_cmpeq_epi8(left0, right0);
        if _mm_movemask_epi8(eq0) != 0xFFFF_i32 {
            return false;
        }

        let left1 = unsafe { _mm_loadu_si128(lhs.add(offset + 16).cast::<__m128i>()) };
        let right1 = unsafe { _mm_loadu_si128(rhs.add(offset + 16).cast::<__m128i>()) };
        let left2 = unsafe { _mm_loadu_si128(lhs.add(offset + 32).cast::<__m128i>()) };
        let right2 = unsafe { _mm_loadu_si128(rhs.add(offset + 32).cast::<__m128i>()) };
        let left3 = unsafe { _mm_loadu_si128(lhs.add(offset + 48).cast::<__m128i>()) };
        let right3 = unsafe { _mm_loadu_si128(rhs.add(offset + 48).cast::<__m128i>()) };

        let eq1 = _mm_cmpeq_epi8(left1, right1);
        let eq2 = _mm_cmpeq_epi8(left2, right2);
        let eq3 = _mm_cmpeq_epi8(left3, right3);
        let eq12 = _mm_and_si128(eq1, eq2);
        let eq123 = _mm_and_si128(eq12, eq3);
        if _mm_movemask_epi8(eq123) != 0xFFFF_i32 {
            return false;
        }
        offset += 64;
    }

    while offset + 16 <= len {
        let left = unsafe { _mm_loadu_si128(lhs.add(offset).cast::<__m128i>()) };
        let right = unsafe { _mm_loadu_si128(rhs.add(offset).cast::<__m128i>()) };
        let equals = _mm_cmpeq_epi8(left, right);
        if _mm_movemask_epi8(equals) != 0xFFFF_i32 {
            return false;
        }
        offset += 16;
    }

    unsafe { row_equal_scalar(lhs.add(offset), rhs.add(offset), len - offset) }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn row_equal_sse2_unrolled(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{__m128i, _mm_cmpeq_epi8, _mm_loadu_si128, _mm_movemask_epi8};

    if len >= 16 {
        let tail_offset = len - 16;
        let tail_left = unsafe { _mm_loadu_si128(lhs.add(tail_offset).cast::<__m128i>()) };
        let tail_right = unsafe { _mm_loadu_si128(rhs.add(tail_offset).cast::<__m128i>()) };
        if _mm_movemask_epi8(_mm_cmpeq_epi8(tail_left, tail_right)) != 0xFFFF_i32 {
            return false;
        }

        if len >= 64 {
            let mid_offset = (len >> 1) & !15usize;
            if mid_offset != tail_offset {
                let mid_left = unsafe { _mm_loadu_si128(lhs.add(mid_offset).cast::<__m128i>()) };
                let mid_right = unsafe { _mm_loadu_si128(rhs.add(mid_offset).cast::<__m128i>()) };
                if _mm_movemask_epi8(_mm_cmpeq_epi8(mid_left, mid_right)) != 0xFFFF_i32 {
                    return false;
                }
            }
        }
    }

    unsafe { row_equal_sse2_unrolled_baseline(lhs, rhs, len) }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn row_equal_avx2_baseline(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{__m256i, _mm256_cmpeq_epi8, _mm256_loadu_si256, _mm256_movemask_epi8};

    let mut offset = 0usize;
    while offset + 32 <= len {
        let left = unsafe { _mm256_loadu_si256(lhs.add(offset).cast::<__m256i>()) };
        let right = unsafe { _mm256_loadu_si256(rhs.add(offset).cast::<__m256i>()) };
        let equals = _mm256_cmpeq_epi8(left, right);
        if _mm256_movemask_epi8(equals) != -1 {
            return false;
        }
        offset += 32;
    }

    unsafe { row_equal_scalar(lhs.add(offset), rhs.add(offset), len - offset) }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn row_equal_avx2_unrolled_baseline(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{
        __m256i, _mm256_loadu_si256, _mm256_or_si256, _mm256_testz_si256, _mm256_xor_si256,
    };

    let mut offset = 0usize;
    while offset + 128 <= len {
        let left0 = unsafe { _mm256_loadu_si256(lhs.add(offset).cast::<__m256i>()) };
        let right0 = unsafe { _mm256_loadu_si256(rhs.add(offset).cast::<__m256i>()) };
        let diff0 = _mm256_xor_si256(left0, right0);
        if _mm256_testz_si256(diff0, diff0) == 0 {
            return false;
        }

        let left1 = unsafe { _mm256_loadu_si256(lhs.add(offset + 32).cast::<__m256i>()) };
        let right1 = unsafe { _mm256_loadu_si256(rhs.add(offset + 32).cast::<__m256i>()) };
        let left2 = unsafe { _mm256_loadu_si256(lhs.add(offset + 64).cast::<__m256i>()) };
        let right2 = unsafe { _mm256_loadu_si256(rhs.add(offset + 64).cast::<__m256i>()) };
        let left3 = unsafe { _mm256_loadu_si256(lhs.add(offset + 96).cast::<__m256i>()) };
        let right3 = unsafe { _mm256_loadu_si256(rhs.add(offset + 96).cast::<__m256i>()) };

        let diff1 = _mm256_xor_si256(left1, right1);
        let diff2 = _mm256_xor_si256(left2, right2);
        let diff3 = _mm256_xor_si256(left3, right3);
        let diff23 = _mm256_or_si256(diff2, diff3);
        let diff_all = _mm256_or_si256(diff1, diff23);
        if _mm256_testz_si256(diff_all, diff_all) == 0 {
            return false;
        }
        offset += 128;
    }

    while offset + 32 <= len {
        let left = unsafe { _mm256_loadu_si256(lhs.add(offset).cast::<__m256i>()) };
        let right = unsafe { _mm256_loadu_si256(rhs.add(offset).cast::<__m256i>()) };
        let diff = _mm256_xor_si256(left, right);
        if _mm256_testz_si256(diff, diff) == 0 {
            return false;
        }
        offset += 32;
    }

    unsafe { row_equal_scalar(lhs.add(offset), rhs.add(offset), len - offset) }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn row_equal_avx2_unrolled(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    use std::arch::x86_64::{__m256i, _mm256_loadu_si256, _mm256_testz_si256, _mm256_xor_si256};

    if len >= 32 {
        let tail_offset = len - 32;
        let tail_left = unsafe { _mm256_loadu_si256(lhs.add(tail_offset).cast::<__m256i>()) };
        let tail_right = unsafe { _mm256_loadu_si256(rhs.add(tail_offset).cast::<__m256i>()) };
        let tail_diff = _mm256_xor_si256(tail_left, tail_right);
        if _mm256_testz_si256(tail_diff, tail_diff) == 0 {
            return false;
        }

        if len >= 128 {
            let mid_offset = (len >> 1) & !31usize;
            if mid_offset != tail_offset {
                let mid_left = unsafe { _mm256_loadu_si256(lhs.add(mid_offset).cast::<__m256i>()) };
                let mid_right =
                    unsafe { _mm256_loadu_si256(rhs.add(mid_offset).cast::<__m256i>()) };
                let mid_diff = _mm256_xor_si256(mid_left, mid_right);
                if _mm256_testz_si256(mid_diff, mid_diff) == 0 {
                    return false;
                }
            }
        }
    }

    unsafe { row_equal_avx2_unrolled_baseline(lhs, rhs, len) }
}

#[inline(always)]
fn window_capture_order(mode: CaptureMode) -> &'static [WindowCapturePath; 4] {
    match mode {
        CaptureMode::Snapshot => &WINDOW_CAPTURE_ORDER_SCREENSHOT,
        CaptureMode::Continuous => &WINDOW_CAPTURE_ORDER_RECORDING,
    }
}

fn build_window_capture_attempts(
    mode: CaptureMode,
    preferred_path: Option<WindowCapturePath>,
) -> ([WindowCapturePath; 4], usize) {
    let default_order = window_capture_order(mode);
    let mut attempts = [default_order[0]; 4];
    let mut attempt_count = 0usize;

    if let Some(preferred) = preferred_path {
        attempts[attempt_count] = preferred;
        attempt_count += 1;
    }

    for &candidate in default_order {
        if attempts[..attempt_count].contains(&candidate) {
            continue;
        }
        attempts[attempt_count] = candidate;
        attempt_count += 1;
    }

    (attempts, attempt_count)
}

struct GdiResources {
    screen_dc: HDC,
    screen_dc_owned: bool,
    mem_dc: HDC,
    capture_bitmap: Option<HBITMAP>,
    history_bitmap: Option<HBITMAP>,
    original_bitmap: Option<HGDIOBJ>,
    window_dc: Option<HDC>,
    window_dc_owner: Option<HWND>,
    bits: *mut u8,
    history_bits: *mut u8,
    history_surface_valid: bool,
    width: i32,
    height: i32,
    stride: usize,
    incremental_duplicate_hint: bool,
    incremental_too_dirty_hint: bool,
    parallel_span_mode_hint: Option<ParallelSpanScanMode>,
    bgra_history: Vec<u8>,
    dirty_row_flags: Vec<u8>,
    dirty_row_runs: Vec<DirtyRowRun>,
    dirty_row_spans: Vec<DirtyRowSpan>,
    dirty_span_runs: Vec<DirtySpanRun>,
}

impl GdiResources {
    fn new() -> CaptureResult<Self> {
        let screen_dc = unsafe { GetDC(None) };
        if screen_dc.0.is_null() {
            return Err(CaptureError::platform(anyhow::anyhow!(
                "GetDC(NULL) returned null"
            )));
        }
        Self::new_with_screen_dc(screen_dc, false)
    }

    fn new_for_monitor(handle: HMONITOR) -> CaptureResult<Self> {
        let screen_dc = create_monitor_screen_dc(handle)?;
        Self::new_with_screen_dc(screen_dc, true)
    }

    fn new_for_device(device_name: &[u16]) -> CaptureResult<Self> {
        let screen_dc = create_monitor_screen_dc_for_device(device_name)?;
        Self::new_with_screen_dc(screen_dc, true)
    }

    fn new_with_screen_dc(screen_dc: HDC, screen_dc_owned: bool) -> CaptureResult<Self> {
        let mem_dc = unsafe { CreateCompatibleDC(Some(screen_dc)) };
        if mem_dc.0.is_null() {
            unsafe {
                if screen_dc_owned {
                    let _ = DeleteDC(screen_dc);
                } else {
                    let _ = ReleaseDC(None, screen_dc);
                }
            }
            return Err(CaptureError::platform(anyhow::anyhow!(
                "CreateCompatibleDC failed"
            )));
        }

        Ok(Self {
            screen_dc,
            screen_dc_owned,
            mem_dc,
            capture_bitmap: None,
            history_bitmap: None,
            original_bitmap: None,
            window_dc: None,
            window_dc_owner: None,
            bits: null_mut(),
            history_bits: null_mut(),
            history_surface_valid: false,
            width: 0,
            height: 0,
            stride: 0,
            incremental_duplicate_hint: false,
            incremental_too_dirty_hint: false,
            parallel_span_mode_hint: None,
            bgra_history: Vec::new(),
            dirty_row_flags: Vec::new(),
            dirty_row_runs: Vec::new(),
            dirty_row_spans: Vec::new(),
            dirty_span_runs: Vec::new(),
        })
    }

    fn release_screen_dc(&mut self) {
        if self.screen_dc.0.is_null() {
            return;
        }
        unsafe {
            if self.screen_dc_owned {
                let _ = DeleteDC(self.screen_dc);
            } else {
                let _ = ReleaseDC(None, self.screen_dc);
            }
        }
        self.screen_dc = HDC(null_mut());
    }

    fn release_window_dc(&mut self) {
        if let (Some(owner), Some(window_dc)) = (self.window_dc_owner.take(), self.window_dc.take())
            && !window_dc.0.is_null()
        {
            unsafe {
                let _ = ReleaseDC(Some(owner), window_dc);
            }
        }
    }

    fn release_idle_resources(&mut self) {
        self.release_window_dc();
        self.release_bitmap();
    }

    fn acquire_window_dc(&mut self, hwnd: HWND) -> CaptureResult<HDC> {
        if let (Some(owner), Some(window_dc)) = (self.window_dc_owner, self.window_dc)
            && owner == hwnd
            && !window_dc.0.is_null()
        {
            return Ok(window_dc);
        }

        self.release_window_dc();

        let window_dc = unsafe { GetWindowDC(Some(hwnd)) };
        if window_dc.0.is_null() {
            return Err(CaptureError::platform(anyhow::anyhow!(
                "GetWindowDC returned null during GDI window capture"
            )));
        }

        self.window_dc_owner = Some(hwnd);
        self.window_dc = Some(window_dc);
        Ok(window_dc)
    }

    fn create_dib_bitmap(&self, width: i32, height: i32) -> CaptureResult<(HBITMAP, *mut u8)> {
        let mut info = BITMAPINFO::default();
        info.bmiHeader.biSize = size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = width;

        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB.0;

        let mut bits: *mut c_void = null_mut();
        let bitmap = unsafe {
            CreateDIBSection(
                Some(self.mem_dc),
                &info,
                DIB_RGB_COLORS,
                &mut bits,
                Some(HANDLE::default()),
                0,
            )
        }
        .context("CreateDIBSection failed")
        .map_err(CaptureError::platform)?;
        if bits.is_null() {
            unsafe {
                let _ = DeleteObject(bitmap.into());
            }
            return Err(CaptureError::platform(anyhow::anyhow!(
                "CreateDIBSection returned a null pixel buffer"
            )));
        }
        Ok((bitmap, bits.cast()))
    }

    fn ensure_history_surface(&mut self) -> CaptureResult<()> {
        if self.width <= 0 || self.height <= 0 {
            return Ok(());
        }
        if self.history_bitmap.is_some() && !self.history_bits.is_null() {
            return Ok(());
        }

        let (history_bitmap, history_bits) = self.create_dib_bitmap(self.width, self.height)?;
        self.history_bitmap = Some(history_bitmap);
        self.history_bits = history_bits;
        self.history_surface_valid = false;
        Ok(())
    }

    fn swap_capture_and_history_surfaces(&mut self) -> CaptureResult<()> {
        self.ensure_history_surface()?;

        let Some(current_bitmap) = self.capture_bitmap else {
            return Ok(());
        };
        let Some(next_bitmap) = self.history_bitmap else {
            return Ok(());
        };
        if current_bitmap == next_bitmap {
            return Ok(());
        }

        let selected = unsafe { SelectObject(self.mem_dc, next_bitmap.into()) };
        if selected.0.is_null() {
            return Err(CaptureError::platform(anyhow::anyhow!(
                "SelectObject failed during gdi history surface swap"
            )));
        }
        if selected.0 != current_bitmap.0 {
            unsafe {
                let _ = SelectObject(self.mem_dc, current_bitmap.into());
            }
            return Err(CaptureError::platform(anyhow::anyhow!(
                "unexpected object selected during gdi history surface swap"
            )));
        }

        self.capture_bitmap = Some(next_bitmap);
        self.history_bitmap = Some(current_bitmap);
        std::mem::swap(&mut self.bits, &mut self.history_bits);
        self.history_surface_valid = true;
        Ok(())
    }

    fn ensure_surface(&mut self, width: i32, height: i32) -> CaptureResult<()> {
        if width <= 0 || height <= 0 {
            return Err(CaptureError::platform(anyhow::anyhow!(
                "invalid gdi surface size {width}x{height}"
            )));
        }

        if self.capture_bitmap.is_some() && self.width == width && self.height == height {
            return Ok(());
        }

        self.release_bitmap();

        let (bitmap, bits) = self.create_dib_bitmap(width, height)?;

        let selected = unsafe { SelectObject(self.mem_dc, bitmap.into()) };
        if selected.0.is_null() {
            unsafe {
                let _ = DeleteObject(bitmap.into());
            }
            return Err(CaptureError::platform(anyhow::anyhow!(
                "SelectObject failed for gdi capture bitmap"
            )));
        }

        self.capture_bitmap = Some(bitmap);
        self.original_bitmap = Some(selected);
        self.bits = bits;
        self.history_surface_valid = false;
        self.width = width;
        self.height = height;
        self.stride = usize::try_from(width)
            .ok()
            .and_then(|w| w.checked_mul(4))
            .ok_or(CaptureError::BufferOverflow)?;
        self.incremental_duplicate_hint = false;
        self.incremental_too_dirty_hint = false;
        self.parallel_span_mode_hint = None;
        Ok(())
    }

    fn ensure_bgra_history(&mut self, byte_len: usize) -> bool {
        if byte_len == 0 {
            self.bgra_history.clear();
            return false;
        }

        let had_history = self.bgra_history.len() == byte_len;
        if !had_history {
            self.bgra_history.resize(byte_len, 0);
        }
        had_history
    }

    fn ensure_dirty_row_flags(&mut self, row_count: usize) {
        if self.dirty_row_flags.len() != row_count {
            self.dirty_row_flags.resize(row_count, 0);
        }
    }

    fn ensure_dirty_row_spans(&mut self, row_count: usize) {
        if self.dirty_row_spans.len() != row_count {
            self.dirty_row_spans
                .resize(row_count, DirtyRowSpan::default());
        }
    }

    fn build_dirty_row_runs_from_flags(&mut self, height: usize) {
        build_dirty_row_runs_from_flags(&self.dirty_row_flags[..height], &mut self.dirty_row_runs);
    }

    fn build_dirty_span_runs_from_spans(&mut self, height: usize) {
        build_dirty_span_runs_from_spans(
            &self.dirty_row_spans[..height],
            &mut self.dirty_span_runs,
        );
    }

    fn scan_dirty_row_runs_parallel(
        &mut self,
        row_bytes: usize,
        height: usize,
        history_base: *mut u8,
        history_stride: usize,
        copy_changed_rows_to_history: bool,
        compare_row: RowCompareKernel,
    ) -> DirtyScanResult {
        self.ensure_dirty_row_flags(height);
        let dirty_rows = scan_dirty_row_flags_parallel(
            &mut self.dirty_row_flags[..height],
            self.bits.cast_const(),
            self.stride,
            history_base,
            history_stride,
            row_bytes,
            compare_row,
            copy_changed_rows_to_history,
        );

        self.build_dirty_row_runs_from_flags(height);
        self.dirty_span_runs.clear();
        DirtyScanResult {
            dirty_rows,
            used_spans: false,
        }
    }

    fn scan_dirty_span_runs_parallel(
        &mut self,
        row_bytes: usize,
        width: usize,
        height: usize,
        history_base: *mut u8,
        history_stride: usize,
        copy_changed_rows_to_history: bool,
        compare_row: RowCompareKernel,
        duplicate_probe_hint: bool,
        max_dirty_pixels: usize,
    ) -> CaptureResult<Option<DirtyScanResult>> {
        self.dirty_row_runs.clear();
        self.dirty_span_runs.clear();
        if width > u32::MAX as usize {
            return Err(CaptureError::BufferOverflow);
        }
        self.ensure_dirty_row_spans(height);
        self.dirty_row_spans[..height].fill(DirtyRowSpan::default());
        let mode_hint = self.parallel_span_mode_hint;
        let scan_mode = select_parallel_span_scan_mode(
            self.bits.cast_const(),
            self.stride,
            history_base.cast_const(),
            history_stride,
            row_bytes,
            height,
            compare_row,
            duplicate_probe_hint,
            mode_hint,
        );

        let (dirty_rows, dirty_pixels) = scan_dirty_row_spans_parallel(
            &mut self.dirty_row_spans[..height],
            self.bits.cast_const(),
            self.stride,
            history_base,
            history_stride,
            row_bytes,
            width,
            compare_row,
            row_diff_bounds_kernel(),
            copy_changed_rows_to_history,
            scan_mode,
        );
        self.parallel_span_mode_hint = Some(recommend_parallel_span_scan_mode(
            dirty_rows,
            dirty_pixels,
            width,
            height,
        ));

        if dirty_rows == 0 {
            return Ok(Some(DirtyScanResult {
                dirty_rows: 0,
                used_spans: true,
            }));
        }
        if dirty_pixels > max_dirty_pixels {
            self.dirty_span_runs.clear();
            return Ok(None);
        }

        self.build_dirty_span_runs_from_spans(height);
        Ok(Some(DirtyScanResult {
            dirty_rows,
            used_spans: true,
        }))
    }

    fn scan_dirty_row_runs(
        &mut self,
        row_bytes: usize,
        height: usize,
        history_base: *mut u8,
        history_stride: usize,
        copy_changed_rows_to_history: bool,
        duplicate_probe_hint: bool,
    ) -> CaptureResult<Option<DirtyScanResult>> {
        if history_base.is_null() {
            return Ok(None);
        }
        if history_stride < row_bytes {
            return Err(CaptureError::BufferOverflow);
        }
        if !row_bytes.is_multiple_of(BGRA_BYTES_PER_PIXEL) {
            return Err(CaptureError::BufferOverflow);
        }

        let compare_row = row_compare_kernel();
        let width = row_bytes / BGRA_BYTES_PER_PIXEL;
        let pixel_count = width
            .checked_mul(height)
            .ok_or(CaptureError::BufferOverflow)?;
        if duplicate_probe_hint
            && incremental_duplicate_probe_eligible(
                pixel_count,
                row_bytes,
                self.stride,
                history_stride,
            )
        {
            let total_bytes = row_bytes
                .checked_mul(height)
                .ok_or(CaptureError::BufferOverflow)?;
            let is_duplicate = unsafe {
                compare_row(
                    self.bits.cast_const(),
                    history_base.cast_const(),
                    total_bytes,
                )
            };
            if is_duplicate {
                self.dirty_row_runs.clear();
                self.dirty_span_runs.clear();
                self.parallel_span_mode_hint = Some(ParallelSpanScanMode::CompareThenDiff);
                return Ok(Some(DirtyScanResult {
                    dirty_rows: 0,
                    used_spans: false,
                }));
            }
        }
        let max_dirty_rows = height.saturating_mul(GDI_INCREMENTAL_MAX_DIRTY_ROW_NUMERATOR)
            / GDI_INCREMENTAL_MAX_DIRTY_ROW_DENOMINATOR;
        let max_dirty_pixels = pixel_count.saturating_mul(GDI_INCREMENTAL_MAX_DIRTY_ROW_NUMERATOR)
            / GDI_INCREMENTAL_MAX_DIRTY_ROW_DENOMINATOR;
        let spans_enabled = incremental_span_convert_eligible(pixel_count, width, height);
        let parallel_rows_eligible = parallel_row_scan_eligible(pixel_count, height);
        let parallel_spans_eligible = parallel_span_scan_eligible(
            pixel_count,
            width,
            height,
            self.incremental_too_dirty_hint,
        );
        if incremental_too_dirty_probe_eligible(pixel_count, height)
            && self.incremental_too_dirty_hint
            && unsafe {
                incremental_too_dirty_probe(
                    self.bits.cast_const(),
                    self.stride,
                    history_base.cast_const(),
                    history_stride,
                    row_bytes,
                    height,
                    compare_row,
                )
            }
        {
            self.dirty_row_runs.clear();
            self.dirty_span_runs.clear();
            self.parallel_span_mode_hint = Some(ParallelSpanScanMode::SingleScanDiff);
            return Ok(None);
        }

        if parallel_spans_eligible {
            return self.scan_dirty_span_runs_parallel(
                row_bytes,
                width,
                height,
                history_base,
                history_stride,
                copy_changed_rows_to_history,
                compare_row,
                duplicate_probe_hint,
                max_dirty_pixels,
            );
        }

        if parallel_rows_eligible {
            let result = self.scan_dirty_row_runs_parallel(
                row_bytes,
                height,
                history_base,
                history_stride,
                copy_changed_rows_to_history,
                compare_row,
            );
            if result.dirty_rows > max_dirty_rows {
                self.dirty_row_runs.clear();
                self.dirty_span_runs.clear();
                return Ok(None);
            }
            return Ok(Some(result));
        }

        self.dirty_row_runs.clear();
        self.dirty_span_runs.clear();
        if spans_enabled {
            self.ensure_dirty_row_spans(height);
            self.dirty_row_spans[..height].fill(DirtyRowSpan::default());
        }

        let mut dirty_rows = 0usize;
        let mut dirty_pixels = 0usize;
        let mut run_start = None::<usize>;
        let mut src_row = self.bits.cast_const();
        let mut history_row = history_base;
        let span_single_scan = spans_enabled;

        for row in 0..height {
            let mut dirty_span = None::<(usize, usize)>;
            let changed = if spans_enabled {
                if span_single_scan {
                    dirty_span = unsafe {
                        row_diff_span_pixels(src_row, history_row.cast_const(), row_bytes, width)
                    };
                    dirty_span.is_some()
                } else {
                    unsafe { !compare_row(src_row, history_row.cast_const(), row_bytes) }
                }
            } else {
                unsafe { !compare_row(src_row, history_row.cast_const(), row_bytes) }
            };
            if !changed {
                if !spans_enabled && let Some(start) = run_start.take() {
                    self.dirty_row_runs.push(DirtyRowRun {
                        start_row: start,
                        row_count: row - start,
                    });
                }
            } else {
                dirty_rows = dirty_rows.saturating_add(1);

                let mut span_start_col = 0usize;
                let row_dirty_pixels = if spans_enabled {
                    let (start_col, end_col) = if let Some(span) = dirty_span {
                        span
                    } else {
                        unsafe {
                            row_diff_span_pixels(
                                src_row,
                                history_row.cast_const(),
                                row_bytes,
                                width,
                            )
                        }
                        .unwrap_or((0, width))
                    };
                    let span_width = end_col.saturating_sub(start_col);
                    let start_col_u32 =
                        u32::try_from(start_col).map_err(|_| CaptureError::BufferOverflow)?;
                    let span_width_u32 =
                        u32::try_from(span_width).map_err(|_| CaptureError::BufferOverflow)?;
                    span_start_col = start_col;
                    self.dirty_row_spans[row] = DirtyRowSpan {
                        start_col: start_col_u32,
                        width: span_width_u32,
                    };
                    span_width
                } else {
                    width
                };
                dirty_pixels = dirty_pixels.saturating_add(row_dirty_pixels);

                if copy_changed_rows_to_history {
                    if spans_enabled && span_single_scan {
                        let span_start_bytes = span_start_col
                            .checked_mul(BGRA_BYTES_PER_PIXEL)
                            .ok_or(CaptureError::BufferOverflow)?;
                        let copy_bytes = row_dirty_pixels
                            .checked_mul(BGRA_BYTES_PER_PIXEL)
                            .ok_or(CaptureError::BufferOverflow)?;
                        let span_end_bytes = span_start_bytes
                            .checked_add(copy_bytes)
                            .ok_or(CaptureError::BufferOverflow)?;
                        if span_end_bytes > row_bytes {
                            return Err(CaptureError::BufferOverflow);
                        }
                        if copy_bytes > 0 {
                            unsafe {
                                std::ptr::copy_nonoverlapping(
                                    src_row.add(span_start_bytes),
                                    history_row.add(span_start_bytes),
                                    copy_bytes,
                                );
                            }
                        }
                    } else {
                        unsafe {
                            std::ptr::copy_nonoverlapping(src_row, history_row, row_bytes);
                        }
                    }
                }
                if !spans_enabled && run_start.is_none() {
                    run_start = Some(row);
                }

                if spans_enabled {
                    if dirty_pixels > max_dirty_pixels {
                        self.dirty_span_runs.clear();
                        return Ok(None);
                    }
                } else if dirty_rows > max_dirty_rows {
                    return Ok(None);
                }
            }

            unsafe {
                src_row = src_row.add(self.stride);
                history_row = history_row.add(history_stride);
            }
        }

        if spans_enabled {
            self.build_dirty_span_runs_from_spans(height);
        } else if let Some(start) = run_start {
            self.dirty_row_runs.push(DirtyRowRun {
                start_row: start,
                row_count: height - start,
            });
        }

        Ok(Some(DirtyScanResult {
            dirty_rows,
            used_spans: spans_enabled,
        }))
    }

    fn convert_dirty_row_runs_into(
        &mut self,
        dst_ptr: *mut u8,
        dst_pitch: usize,
        width: usize,
    ) -> CaptureResult<()> {
        let converter = bgra_row_converter();
        let src_base = self.bits.cast_const();
        let dst_addr = dst_ptr as usize;

        for run in &self.dirty_row_runs {
            let src_offset = run
                .start_row
                .checked_mul(self.stride)
                .ok_or(CaptureError::BufferOverflow)?;
            let dst_offset = run
                .start_row
                .checked_mul(dst_pitch)
                .ok_or(CaptureError::BufferOverflow)?;
            let src_ptr = unsafe { src_base.add(src_offset) };
            let dst_ptr = dst_addr
                .checked_add(dst_offset)
                .ok_or(CaptureError::BufferOverflow)? as *mut u8;
            unsafe {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_ptr,
                    self.stride,
                    dst_ptr,
                    dst_pitch,
                    width,
                    run.row_count,
                );
            }
        }
        Ok(())
    }

    fn convert_dirty_span_runs_into(
        &mut self,
        dst_ptr: *mut u8,
        dst_pitch: usize,
    ) -> CaptureResult<()> {
        let converter = bgra_row_converter();
        let src_base = self.bits.cast_const();
        let dst_addr = dst_ptr as usize;

        for run in &self.dirty_span_runs {
            let src_offset = run
                .start_row
                .checked_mul(self.stride)
                .and_then(|offset| {
                    run.start_col
                        .checked_mul(BGRA_BYTES_PER_PIXEL)
                        .and_then(|xoff| offset.checked_add(xoff))
                })
                .ok_or(CaptureError::BufferOverflow)?;
            let dst_offset = run
                .start_row
                .checked_mul(dst_pitch)
                .and_then(|offset| {
                    run.start_col
                        .checked_mul(BGRA_BYTES_PER_PIXEL)
                        .and_then(|xoff| offset.checked_add(xoff))
                })
                .ok_or(CaptureError::BufferOverflow)?;
            let src_ptr = unsafe { src_base.add(src_offset) };
            let dst_ptr = dst_addr
                .checked_add(dst_offset)
                .ok_or(CaptureError::BufferOverflow)? as *mut u8;
            unsafe {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_ptr,
                    self.stride,
                    dst_ptr,
                    dst_pitch,
                    run.width,
                    run.row_count,
                );
            }
        }

        Ok(())
    }

    fn try_convert_incremental_rows_with_vec_history_into(
        &mut self,
        dst_ptr: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
    ) -> CaptureResult<IncrementalConvertStatus> {
        let row_bytes = width
            .checked_mul(BGRA_BYTES_PER_PIXEL)
            .ok_or(CaptureError::BufferOverflow)?;
        if dst_pitch < row_bytes {
            return Err(CaptureError::BufferOverflow);
        }
        let total_bytes = row_bytes
            .checked_mul(height)
            .ok_or(CaptureError::BufferOverflow)?;
        if !self.ensure_bgra_history(total_bytes) {
            return Ok(IncrementalConvertStatus::NotAvailable);
        }

        let history_ptr = self.bgra_history.as_mut_ptr();
        let dirty_scan = self.scan_dirty_row_runs(
            row_bytes,
            height,
            history_ptr,
            row_bytes,
            true,
            self.incremental_duplicate_hint,
        )?;
        let Some(dirty_scan) = dirty_scan else {
            return Ok(IncrementalConvertStatus::TooDirty);
        };
        if dirty_scan.dirty_rows == 0 {
            return Ok(IncrementalConvertStatus::Duplicate);
        }

        if dirty_scan.used_spans {
            self.convert_dirty_span_runs_into(dst_ptr, dst_pitch)?;
        } else {
            self.convert_dirty_row_runs_into(dst_ptr, dst_pitch, width)?;
        }
        Ok(IncrementalConvertStatus::Updated(dirty_scan.dirty_rows))
    }

    fn try_convert_incremental_rows_with_surface_history_into(
        &mut self,
        dst_ptr: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
    ) -> CaptureResult<IncrementalConvertStatus> {
        let row_bytes = width
            .checked_mul(BGRA_BYTES_PER_PIXEL)
            .ok_or(CaptureError::BufferOverflow)?;
        if dst_pitch < row_bytes {
            return Err(CaptureError::BufferOverflow);
        }
        if !self.history_surface_valid || self.history_bits.is_null() {
            return Ok(IncrementalConvertStatus::NotAvailable);
        }

        let dirty_scan = self.scan_dirty_row_runs(
            row_bytes,
            height,
            self.history_bits,
            self.stride,
            false,
            self.incremental_duplicate_hint,
        )?;
        let Some(dirty_scan) = dirty_scan else {
            return Ok(IncrementalConvertStatus::TooDirty);
        };
        if dirty_scan.dirty_rows == 0 {
            return Ok(IncrementalConvertStatus::Duplicate);
        }

        if dirty_scan.used_spans {
            self.convert_dirty_span_runs_into(dst_ptr, dst_pitch)?;
        } else {
            self.convert_dirty_row_runs_into(dst_ptr, dst_pitch, width)?;
        }
        Ok(IncrementalConvertStatus::Updated(dirty_scan.dirty_rows))
    }

    fn commit_incremental_history(&mut self, total_bytes: usize) -> CaptureResult<()> {
        let _ = total_bytes;
        self.swap_capture_and_history_surfaces()?;
        Ok(())
    }

    /// Capture the monitor region and return an RGBA frame.
    ///
    /// The strategy is to BitBlt into the DIB section, then perform an
    /// bulk-copy the result into the `Frame`.  When `src == dst` the
    /// SIMD kernels read and write the same cache lines, cutting memory
    fn read_surface_to_rgba(
        &mut self,
        width: i32,
        height: i32,
        reuse: Option<Frame>,
        mode: CaptureMode,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        let width_u32 = u32::try_from(width).map_err(|_| CaptureError::BufferOverflow)?;
        let height_u32 = u32::try_from(height).map_err(|_| CaptureError::BufferOverflow)?;
        let width = usize::try_from(width_u32).map_err(|_| CaptureError::BufferOverflow)?;
        let height = usize::try_from(height_u32).map_err(|_| CaptureError::BufferOverflow)?;
        let pixel_count = width
            .checked_mul(height)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_pitch = width.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;

        let mut frame = reuse.unwrap_or_else(Frame::empty);
        frame.reset_metadata();
        frame.ensure_rgba_capacity(width_u32, height_u32)?;
        let track_incremental_history =
            mode != CaptureMode::Snapshot && pixel_count >= GDI_INCREMENTAL_MIN_PIXELS;
        let total_bytes = if track_incremental_history {
            Some(
                width
                    .checked_mul(height)
                    .and_then(|pixels| pixels.checked_mul(4))
                    .ok_or(CaptureError::BufferOverflow)?,
            )
        } else {
            None
        };
        let incremental_enabled = track_incremental_history && destination_has_history;
        let use_surface_history = track_incremental_history;
        let mut next_too_dirty_hint = false;
        if incremental_enabled {
            let incremental_status = if use_surface_history {
                self.try_convert_incremental_rows_with_surface_history_into(
                    frame.as_mut_rgba_ptr(),
                    dst_pitch,
                    width,
                    height,
                )
            } else {
                self.try_convert_incremental_rows_with_vec_history_into(
                    frame.as_mut_rgba_ptr(),
                    dst_pitch,
                    width,
                    height,
                )
            }?;

            match incremental_status {
                IncrementalConvertStatus::Duplicate => {
                    self.incremental_duplicate_hint = true;
                    self.incremental_too_dirty_hint = false;
                    frame.metadata.is_duplicate = true;
                    return Ok(frame);
                }
                IncrementalConvertStatus::Updated(_) => {
                    self.incremental_duplicate_hint = false;
                    self.incremental_too_dirty_hint = false;
                    if use_surface_history && let Some(total_bytes) = total_bytes {
                        self.commit_incremental_history(total_bytes)?;
                    }
                    return Ok(frame);
                }
                IncrementalConvertStatus::NotAvailable => {
                    self.incremental_duplicate_hint = false;
                }
                IncrementalConvertStatus::TooDirty => {
                    self.incremental_duplicate_hint = false;
                    next_too_dirty_hint = true;
                }
            }
        }

        unsafe {
            convert_gdi_bgra_surface_to_rgba(
                self.bits.cast_const(),
                frame.as_mut_rgba_ptr(),
                pixel_count,
                mode,
                destination_has_history,
            )
        }

        if let Some(total_bytes) = total_bytes {
            self.commit_incremental_history(total_bytes)?;
        }
        self.incremental_duplicate_hint = false;
        self.incremental_too_dirty_hint = next_too_dirty_hint;

        Ok(frame)
    }

    fn capture_to_rgba(
        &mut self,
        geometry: MonitorGeometry,
        reuse: Option<Frame>,
        mode: CaptureMode,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.ensure_surface(geometry.width, geometry.height)?;

        unsafe {
            BitBlt(
                self.mem_dc,
                0,
                0,
                geometry.width,
                geometry.height,
                Some(self.screen_dc),
                geometry.left,
                geometry.top,
                SRCCOPY,
            )
        }
        .context("BitBlt failed during GDI monitor capture")
        .map_err(CaptureError::platform)?;
        self.read_surface_to_rgba(
            geometry.width,
            geometry.height,
            reuse,
            mode,
            destination_has_history,
        )
    }

    /// Capture a desktop-space rectangle and write it directly into an
    /// already allocated destination frame at `dst_x/dst_y`.
    fn capture_rect_into_rgba(
        &mut self,
        source_x: i32,
        source_y: i32,
        copy_width: u32,
        copy_height: u32,
        destination: &mut Frame,
        dst_x: u32,
        dst_y: u32,
        mode: CaptureMode,
        destination_has_history: bool,
    ) -> CaptureResult<bool> {
        if copy_width == 0 || copy_height == 0 {
            return Ok(true);
        }

        let dst_width = destination.width();
        let dst_height = destination.height();
        let dst_right = dst_x
            .checked_add(copy_width)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_bottom = dst_y
            .checked_add(copy_height)
            .ok_or(CaptureError::BufferOverflow)?;
        if dst_right > dst_width || dst_bottom > dst_height {
            return Err(CaptureError::BufferOverflow);
        }

        let copy_w_i32 = i32::try_from(copy_width).map_err(|_| CaptureError::BufferOverflow)?;
        let copy_h_i32 = i32::try_from(copy_height).map_err(|_| CaptureError::BufferOverflow)?;
        self.ensure_surface(copy_w_i32, copy_h_i32)?;

        unsafe {
            BitBlt(
                self.mem_dc,
                0,
                0,
                copy_w_i32,
                copy_h_i32,
                Some(self.screen_dc),
                source_x,
                source_y,
                SRCCOPY,
            )
        }
        .context("BitBlt failed during GDI region capture")
        .map_err(CaptureError::platform)?;

        let copy_w = usize::try_from(copy_width).map_err(|_| CaptureError::BufferOverflow)?;
        let copy_h = usize::try_from(copy_height).map_err(|_| CaptureError::BufferOverflow)?;
        let dst_pitch = usize::try_from(dst_width)
            .map_err(|_| CaptureError::BufferOverflow)?
            .checked_mul(4)
            .ok_or(CaptureError::BufferOverflow)?;
        let dst_height_usize =
            usize::try_from(dst_height).map_err(|_| CaptureError::BufferOverflow)?;
        let dst_required_len = dst_pitch
            .checked_mul(dst_height_usize)
            .ok_or(CaptureError::BufferOverflow)?;
        if destination.as_rgba_bytes().len() < dst_required_len {
            return Err(CaptureError::BufferOverflow);
        }
        let dst_x_usize = usize::try_from(dst_x).map_err(|_| CaptureError::BufferOverflow)?;
        let dst_y_usize = usize::try_from(dst_y).map_err(|_| CaptureError::BufferOverflow)?;
        let dst_offset = dst_y_usize
            .checked_mul(dst_pitch)
            .and_then(|base| {
                dst_x_usize
                    .checked_mul(4)
                    .and_then(|xoff| base.checked_add(xoff))
            })
            .ok_or(CaptureError::BufferOverflow)?;

        let dst_ptr = unsafe { destination.as_mut_rgba_ptr().add(dst_offset) };
        let row_bytes = copy_w.checked_mul(4).ok_or(CaptureError::BufferOverflow)?;
        let pixel_count = copy_w
            .checked_mul(copy_h)
            .ok_or(CaptureError::BufferOverflow)?;
        let track_incremental_history =
            mode != CaptureMode::Snapshot && pixel_count >= GDI_INCREMENTAL_MIN_PIXELS;
        let total_bytes = if track_incremental_history {
            Some(
                pixel_count
                    .checked_mul(4)
                    .ok_or(CaptureError::BufferOverflow)?,
            )
        } else {
            None
        };
        let incremental_enabled = track_incremental_history && destination_has_history;
        let use_surface_history = track_incremental_history;
        let mut next_too_dirty_hint = false;
        if incremental_enabled {
            let incremental_status = if use_surface_history {
                self.try_convert_incremental_rows_with_surface_history_into(
                    dst_ptr, dst_pitch, copy_w, copy_h,
                )
            } else {
                self.try_convert_incremental_rows_with_vec_history_into(
                    dst_ptr, dst_pitch, copy_w, copy_h,
                )
            }?;

            match incremental_status {
                IncrementalConvertStatus::Duplicate => {
                    self.incremental_duplicate_hint = true;
                    self.incremental_too_dirty_hint = false;
                    return Ok(true);
                }
                IncrementalConvertStatus::Updated(_) => {
                    self.incremental_duplicate_hint = false;
                    self.incremental_too_dirty_hint = false;
                    if use_surface_history && let Some(total_bytes) = total_bytes {
                        self.commit_incremental_history(total_bytes)?;
                    }
                    return Ok(false);
                }
                IncrementalConvertStatus::NotAvailable => {
                    self.incremental_duplicate_hint = false;
                }
                IncrementalConvertStatus::TooDirty => {
                    self.incremental_duplicate_hint = false;
                    next_too_dirty_hint = true;
                }
            }
        }

        // Fast path: contiguous destination rows (single-monitor region output)
        // can use the dedicated contiguous kernels directly.
        if dst_pitch == row_bytes {
            unsafe {
                convert_gdi_bgra_surface_to_rgba(
                    self.bits.cast_const(),
                    dst_ptr,
                    pixel_count,
                    mode,
                    destination_has_history,
                )
            }
        } else {
            // General path for multi-monitor composites where destination rows
            // include padding to the full region width.
            unsafe {
                convert::convert_surface_to_rgba_unchecked(
                    convert::SurfacePixelFormat::Bgra8,
                    convert::SurfaceLayout::new(
                        self.bits.cast_const(),
                        self.stride,
                        dst_ptr,
                        dst_pitch,
                        copy_w,
                        copy_h,
                    ),
                    convert::SurfaceConversionOptions {
                        force_opaque_alpha: true,
                        ..convert::SurfaceConversionOptions::default()
                    },
                );
            }
        }

        if let Some(total_bytes) = total_bytes {
            self.commit_incremental_history(total_bytes)?;
        }
        self.incremental_duplicate_hint = false;
        self.incremental_too_dirty_hint = next_too_dirty_hint;

        Ok(false)
    }

    /// Capture a monitor sub-rectangle and write it directly into an
    /// already allocated destination frame at `blit.dst_x/dst_y`.
    ///
    /// This avoids the baseline region fallback path of:
    /// 1) full monitor BitBlt + full monitor BGRA->RGBA conversion
    /// 2) CPU crop/copy into the region output frame
    ///
    /// Instead, we only BitBlt the requested source rectangle and convert
    /// those pixels straight into the destination slice.
    fn capture_region_into_rgba(
        &mut self,
        geometry: MonitorGeometry,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        mode: CaptureMode,
        destination_has_history: bool,
    ) -> CaptureResult<bool> {
        if blit.width == 0 || blit.height == 0 {
            return Ok(true);
        }

        let src_width = u32::try_from(geometry.width).map_err(|_| CaptureError::BufferOverflow)?;
        let src_height =
            u32::try_from(geometry.height).map_err(|_| CaptureError::BufferOverflow)?;
        let src_right = blit
            .src_x
            .checked_add(blit.width)
            .ok_or(CaptureError::BufferOverflow)?;
        let src_bottom = blit
            .src_y
            .checked_add(blit.height)
            .ok_or(CaptureError::BufferOverflow)?;
        if src_right > src_width || src_bottom > src_height {
            return Err(CaptureError::BufferOverflow);
        }

        let source_x = i32::try_from(i64::from(geometry.left) + i64::from(blit.src_x))
            .map_err(|_| CaptureError::BufferOverflow)?;
        let source_y = i32::try_from(i64::from(geometry.top) + i64::from(blit.src_y))
            .map_err(|_| CaptureError::BufferOverflow)?;

        self.capture_rect_into_rgba(
            source_x,
            source_y,
            blit.width,
            blit.height,
            destination,
            blit.dst_x,
            blit.dst_y,
            mode,
            destination_has_history,
        )
    }

    fn capture_desktop_region_into_rgba(
        &mut self,
        source_x: i32,
        source_y: i32,
        width: u32,
        height: u32,
        destination: &mut Frame,
        mode: CaptureMode,
        destination_has_history: bool,
    ) -> CaptureResult<bool> {
        self.capture_rect_into_rgba(
            source_x,
            source_y,
            width,
            height,
            destination,
            0,
            0,
            mode,
            destination_has_history,
        )
    }

    /// Capture a window directly into the backing DIB.
    fn capture_window_to_rgba(
        &mut self,
        hwnd: HWND,
        width: i32,
        height: i32,
        reuse: Option<Frame>,
        mode: CaptureMode,
        preferred_path: Option<WindowCapturePath>,
        destination_has_history: bool,
    ) -> CaptureResult<(Frame, WindowCapturePath)> {
        self.ensure_surface(width, height)?;

        let (attempts, attempt_count) = build_window_capture_attempts(mode, preferred_path);

        let mut last_error: Option<CaptureError> = None;
        for &path in &attempts[..attempt_count] {
            match self.try_capture_window_path(hwnd, width, height, path) {
                Ok(()) => {
                    let frame = self.read_surface_to_rgba(
                        width,
                        height,
                        reuse,
                        mode,
                        destination_has_history,
                    )?;
                    return Ok((frame, path));
                }
                Err(error) => {
                    last_error = Some(error);
                }
            }
        }

        Err(last_error.unwrap_or_else(|| {
            CaptureError::platform(anyhow::anyhow!("all GDI window capture strategies failed"))
        }))
    }

    fn try_capture_window_path(
        &mut self,
        hwnd: HWND,
        width: i32,
        height: i32,
        path: WindowCapturePath,
    ) -> CaptureResult<()> {
        match path {
            WindowCapturePath::WindowDcBitBlt => {
                let window_dc = self.acquire_window_dc(hwnd)?;
                unsafe {
                    BitBlt(
                        self.mem_dc,
                        0,
                        0,
                        width,
                        height,
                        Some(window_dc),
                        0,
                        0,
                        SRCCOPY,
                    )
                }
                .context("BitBlt failed during GDI window capture")
                .map_err(CaptureError::platform)?;
                Ok(())
            }
            WindowCapturePath::PrintWindow(flags) => {
                self.release_window_dc();
                if unsafe { PrintWindow(hwnd, self.mem_dc, flags) }.as_bool() {
                    return Ok(());
                }

                Err(CaptureError::platform(anyhow::anyhow!(
                    "PrintWindow failed for flags {:#x}",
                    flags.0
                )))
            }
        }
    }

    fn release_bitmap(&mut self) {
        if let Some(original_bitmap) = self.original_bitmap.take() {
            unsafe {
                let _ = SelectObject(self.mem_dc, original_bitmap);
            }
        }
        if let Some(capture_bitmap) = self.capture_bitmap.take() {
            unsafe {
                let _ = DeleteObject(capture_bitmap.into());
            }
        }
        if let Some(history_bitmap) = self.history_bitmap.take() {
            unsafe {
                let _ = DeleteObject(history_bitmap.into());
            }
        }
        self.bits = null_mut();
        self.history_bits = null_mut();
        self.history_surface_valid = false;
        self.width = 0;
        self.height = 0;
        self.stride = 0;
        self.incremental_duplicate_hint = false;
        self.incremental_too_dirty_hint = false;
        self.parallel_span_mode_hint = None;
        self.bgra_history.clear();
        self.dirty_row_flags.clear();
        self.dirty_row_runs.clear();
        self.dirty_row_spans.clear();
        self.dirty_span_runs.clear();
    }
}

impl Drop for GdiResources {
    fn drop(&mut self) {
        self.release_window_dc();
        self.release_bitmap();

        if !self.mem_dc.0.is_null() {
            unsafe {
                let _ = DeleteDC(self.mem_dc);
            }
        }
        self.release_screen_dc();
    }
}

pub(crate) struct WindowsMonitorCapturer {
    monitor: MonitorId,
    resolver: Arc<MonitorResolver>,
    resources: GdiResources,
    monitor_source_dc_local: bool,
    geometry: MonitorGeometry,
    capture_mode: CaptureMode,
    /// Tracks the `DisplayInfoCache` generation so we only re-query
    /// monitor geometry when `WM_DISPLAYCHANGE` has actually fired.
    last_display_generation: Option<u64>,
}

// SAFETY: GdiResources contains raw HDC/HBITMAP handles which are not
// Send, but we only access them from the owning capture thread.
// The streaming module ensures single-threaded access.
unsafe impl Send for WindowsMonitorCapturer {}

impl WindowsMonitorCapturer {
    pub(crate) fn new(monitor: &MonitorId, resolver: Arc<MonitorResolver>) -> CaptureResult<Self> {
        let geometry = resolve_geometry(&resolver, monitor)?;
        let (resources, monitor_source_dc_local) =
            match GdiResources::new_for_monitor(geometry.handle) {
                Ok(resources) => (resources, true),
                Err(_) => (GdiResources::new()?, false),
            };

        let last_display_generation = resolver.display_generation();

        Ok(Self {
            monitor: monitor.clone(),
            resolver,
            resources,
            monitor_source_dc_local,
            geometry,
            capture_mode: CaptureMode::Snapshot,
            last_display_generation,
        })
    }

    fn refresh_geometry(&mut self) -> CaptureResult<()> {
        let current_gen = self.resolver.display_generation();

        if let (Some(current), Some(last)) = (current_gen, self.last_display_generation)
            && current == last
        {
            return Ok(());
        }

        self.last_display_generation = current_gen;
        self.geometry = match geometry_from_handle(self.geometry.handle) {
            Ok(geometry) => geometry,
            Err(_) => resolve_geometry(&self.resolver, &self.monitor)?,
        };

        let (resources, monitor_source_dc_local) =
            match GdiResources::new_for_monitor(self.geometry.handle) {
                Ok(resources) => (resources, true),
                Err(_) => (GdiResources::new()?, false),
            };
        self.resources = resources;
        self.monitor_source_dc_local = monitor_source_dc_local;
        Ok(())
    }

    #[inline(always)]
    fn source_geometry(&self) -> MonitorGeometry {
        if self.monitor_source_dc_local {
            MonitorGeometry {
                handle: self.geometry.handle,
                left: 0,
                top: 0,
                width: self.geometry.width,
                height: self.geometry.height,
            }
        } else {
            self.geometry
        }
    }
}

impl crate::backend::MonitorCapturer for WindowsMonitorCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::Gdi
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.capture_with_history_hint(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        self.refresh_geometry()?;
        let capture_time = Instant::now();
        let mut frame = self.resources.capture_to_rgba(
            self.source_geometry(),
            reuse,
            self.capture_mode,
            destination_has_history,
        )?;
        frame
            .metadata
            .set_timing(Some(capture_time), crate::frame::query_qpc_now());
        Ok(frame)
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        self.refresh_geometry()?;
        let capture_time = Instant::now();
        let is_duplicate = self.resources.capture_region_into_rgba(
            self.source_geometry(),
            blit,
            destination,
            self.capture_mode,
            destination_has_history,
        )?;
        Ok(Some(CaptureSampleMetadata {
            capture_time: Some(capture_time),
            raw_os_ticks: crate::frame::query_qpc_now(),
            tick_format: snow_core::timestamp::TickFormat::RawQpc,
            is_duplicate,
            dirty_rects: Vec::new(),
        }))
    }

    fn capture_desktop_region_into(
        &mut self,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        if width == 0 || height == 0 {
            return Ok(Some(CaptureSampleMetadata {
                capture_time: Some(Instant::now()),
                raw_os_ticks: crate::frame::query_qpc_now(),
                tick_format: snow_core::timestamp::TickFormat::RawQpc,
                is_duplicate: true,
                dirty_rects: Vec::new(),
            }));
        }

        self.refresh_geometry()?;
        let capture_time = Instant::now();
        let (source_x, source_y) = if self.monitor_source_dc_local {
            (
                x.checked_sub(self.geometry.left)
                    .ok_or(CaptureError::BufferOverflow)?,
                y.checked_sub(self.geometry.top)
                    .ok_or(CaptureError::BufferOverflow)?,
            )
        } else {
            (x, y)
        };
        let is_duplicate = self.resources.capture_desktop_region_into_rgba(
            source_x,
            source_y,
            width,
            height,
            destination,
            self.capture_mode,
            destination_has_history,
        )?;
        Ok(Some(CaptureSampleMetadata {
            capture_time: Some(capture_time),
            raw_os_ticks: crate::frame::query_qpc_now(),
            tick_format: snow_core::timestamp::TickFormat::RawQpc,
            is_duplicate,
            dirty_rects: Vec::new(),
        }))
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.capture_mode = mode;
        Ok(())
    }

    fn release_capture_access(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_window_dc();
        }
    }

    fn release_idle_resources(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_idle_resources();
        }
    }
}

pub(crate) struct WindowsPrimaryMonitorCapturer {
    resources: GdiResources,
    binding: PrimaryMonitorBinding,
    monitor_source_dc_local: bool,
    capture_mode: CaptureMode,
}

// SAFETY: capture is serialized by CaptureSession, and every GDI handle is
// used only by the thread currently owning the capturer.
unsafe impl Send for WindowsPrimaryMonitorCapturer {}

impl WindowsPrimaryMonitorCapturer {
    pub(crate) fn new() -> CaptureResult<Self> {
        let binding = query_primary_monitor_binding()?;

        let (resources, monitor_source_dc_local) =
            match GdiResources::new_for_device(&binding.device_name) {
                Ok(resources) => (resources, true),
                Err(_) => (GdiResources::new()?, false),
            };

        Ok(Self {
            resources,
            binding,
            monitor_source_dc_local,
            capture_mode: CaptureMode::Snapshot,
        })
    }

    fn target_info_for_binding(binding: &PrimaryMonitorBinding) -> CaptureTargetInfo {
        CaptureTargetInfo {
            origin_x: binding.geometry.left,
            origin_y: binding.geometry.top,
            width: binding.geometry.width as u32,
            height: binding.geometry.height as u32,
        }
    }

    fn source_geometry(&self) -> MonitorGeometry {
        if self.monitor_source_dc_local {
            MonitorGeometry {
                handle: self.binding.geometry.handle,
                left: 0,
                top: 0,
                width: self.binding.geometry.width,
                height: self.binding.geometry.height,
            }
        } else {
            self.binding.geometry
        }
    }

    fn rebind_if_needed(&mut self, binding: PrimaryMonitorBinding) -> CaptureResult<()> {
        if binding == self.binding {
            return Ok(());
        }

        let (resources, monitor_source_dc_local) =
            match GdiResources::new_for_device(&binding.device_name) {
                Ok(resources) => (resources, true),
                Err(_) => (GdiResources::new()?, false),
            };
        self.resources = resources;
        self.binding = binding;
        self.monitor_source_dc_local = monitor_source_dc_local;
        Ok(())
    }
}

impl crate::backend::MonitorCapturer for WindowsPrimaryMonitorCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::Gdi
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.capture_with_history_hint(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        let binding = query_primary_monitor_binding()?;
        self.rebind_if_needed(binding)?;

        let captured_binding = self.binding.clone();
        let capture_time = Instant::now();
        let captured = self.resources.capture_to_rgba(
            self.source_geometry(),
            reuse,
            self.capture_mode,
            destination_has_history,
        );
        let post_binding = query_primary_monitor_binding();
        let binding_changed = post_binding
            .as_ref()
            .map_or(true, |binding| binding != &captured_binding);

        let mut frame = match captured {
            Ok(frame) => frame,
            Err(error) => {
                if binding_changed {
                    return Err(CaptureError::MonitorLost);
                }
                return Err(error);
            }
        };
        if binding_changed {
            return Err(CaptureError::MonitorLost);
        }

        frame
            .metadata
            .set_timing(Some(capture_time), crate::frame::query_qpc_now());
        Ok(frame)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        self.capture_mode = mode;
        Ok(())
    }

    fn target_info(&self) -> Option<CaptureTargetInfo> {
        Some(Self::target_info_for_binding(&self.binding))
    }

    fn release_capture_access(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_window_dc();
        }
    }

    fn release_idle_resources(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_idle_resources();
        }
    }
}

use crate::backend::MonitorCapturer;
use crate::window::WindowId;

pub(crate) struct WindowsWindowCapturer {
    resources: GdiResources,
    hwnd: HWND,
    capture_mode: CaptureMode,
    preferred_path: Option<WindowCapturePath>,
    cached_rect: Option<RECT>,
    frames_until_state_refresh: u32,
}

// GdiResources contains raw pointers (HDC, HBITMAP, *mut u8) that are
// not inherently Send, but we only access them from the owning capture
// thread.  The streaming module ensures single-threaded access.
unsafe impl Send for WindowsWindowCapturer {}

impl WindowsWindowCapturer {
    pub(crate) fn new(window: &WindowId) -> CaptureResult<Self> {
        let hwnd = HWND(window.raw_handle() as *mut std::ffi::c_void);
        if hwnd.0.is_null() {
            return Err(CaptureError::InvalidTarget(format!(
                "window handle is null: {}",
                window.stable_id()
            )));
        }
        if !unsafe { IsWindow(Some(hwnd)) }.as_bool() {
            return Err(CaptureError::InvalidTarget(format!(
                "window handle is not valid: {}",
                window.stable_id()
            )));
        }
        let resources = GdiResources::new()?;
        Ok(Self {
            resources,
            hwnd,
            capture_mode: CaptureMode::Snapshot,
            preferred_path: None,
            cached_rect: None,
            frames_until_state_refresh: 0,
        })
    }

    fn refresh_window_rect(&mut self, require_visible: bool) -> CaptureResult<RECT> {
        if require_visible && !unsafe { IsWindowVisible(self.hwnd) }.as_bool() {
            return Err(CaptureError::InvalidTarget("window is not visible".into()));
        }

        let mut rect = RECT::default();
        unsafe { GetWindowRect(self.hwnd, &mut rect) }
            .ok()
            .context("GetWindowRect failed")
            .map_err(CaptureError::platform)?;

        let width = rect.right.saturating_sub(rect.left);
        let height = rect.bottom.saturating_sub(rect.top);
        if width <= 0 || height <= 0 {
            return Err(CaptureError::InvalidTarget(
                "window has empty bounds".into(),
            ));
        }

        self.cached_rect = Some(rect);
        self.frames_until_state_refresh =
            gdi_window_state_refresh_interval_frames().saturating_sub(1);
        Ok(rect)
    }

    fn resolve_window_rect(&mut self) -> CaptureResult<(RECT, bool)> {
        if self.frames_until_state_refresh > 0 {
            if let Some(rect) = self.cached_rect {
                self.frames_until_state_refresh -= 1;
                return Ok((rect, true));
            }
            self.frames_until_state_refresh = 0;
        }

        self.refresh_window_rect(true).map(|rect| (rect, false))
    }

    fn invalidate_window_state_cache(&mut self) {
        self.cached_rect = None;
        self.frames_until_state_refresh = 0;
    }
}

impl MonitorCapturer for WindowsWindowCapturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::Gdi
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.capture_with_history_hint(reuse, false)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        if !unsafe { IsWindow(Some(self.hwnd)) }.as_bool() {
            return Err(CaptureError::InvalidTarget(
                "window no longer exists".into(),
            ));
        }
        if unsafe { IsIconic(self.hwnd) }.as_bool() {
            self.invalidate_window_state_cache();
            return Err(CaptureError::InvalidTarget("window is minimized".into()));
        }
        if !unsafe { IsWindowVisible(self.hwnd) }.as_bool() {
            self.invalidate_window_state_cache();
            return Err(CaptureError::InvalidTarget("window is not visible".into()));
        }
        let (mut rect, used_cached_rect) = self.resolve_window_rect()?;
        let width = rect.right.saturating_sub(rect.left);
        let height = rect.bottom.saturating_sub(rect.top);
        if width <= 0 || height <= 0 {
            self.invalidate_window_state_cache();
            return Err(CaptureError::InvalidTarget(
                "window has empty bounds".into(),
            ));
        }

        let capture_time = Instant::now();
        let first_attempt = self.resources.capture_window_to_rgba(
            self.hwnd,
            width,
            height,
            reuse,
            self.capture_mode,
            self.preferred_path,
            destination_has_history,
        );
        let (mut frame, used_path) = match first_attempt {
            Ok(result) => result,
            Err(_) if used_cached_rect => {
                rect = self.refresh_window_rect(true)?;
                let retry_width = rect.right.saturating_sub(rect.left);
                let retry_height = rect.bottom.saturating_sub(rect.top);
                if retry_width <= 0 || retry_height <= 0 {
                    self.invalidate_window_state_cache();
                    return Err(CaptureError::InvalidTarget(
                        "window has empty bounds".into(),
                    ));
                }
                self.resources.capture_window_to_rgba(
                    self.hwnd,
                    retry_width,
                    retry_height,
                    None,
                    self.capture_mode,
                    self.preferred_path,
                    false,
                )?
            }
            Err(error) => return Err(error),
        };
        self.preferred_path = Some(used_path);
        frame
            .metadata
            .set_timing(Some(capture_time), crate::frame::query_qpc_now());
        Ok(frame)
    }

    fn set_capture_mode(&mut self, mode: CaptureMode) -> CaptureResult<()> {
        if self.capture_mode != mode {
            self.preferred_path = None;
            self.invalidate_window_state_cache();
        }
        self.capture_mode = mode;
        Ok(())
    }

    fn release_capture_access(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_window_dc();
        }
    }

    fn release_idle_resources(&mut self) {
        if self.capture_mode == CaptureMode::Snapshot {
            self.resources.release_idle_resources();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn screenshot_mode_prefers_window_dc_paths() {
        let (attempts, count) = build_window_capture_attempts(CaptureMode::Snapshot, None);
        assert_eq!(count, 4);
        assert_eq!(attempts[0], WindowCapturePath::WindowDcBitBlt);
        assert_eq!(
            attempts[1],
            WindowCapturePath::PrintWindow(PRINT_WINDOW_RENDER_FULL)
        );
        assert_eq!(
            attempts[2],
            WindowCapturePath::PrintWindow(PRINT_WINDOW_DEFAULT)
        );
        assert_eq!(
            attempts[3],
            WindowCapturePath::PrintWindow(PRINT_WINDOW_EXPERIMENTAL)
        );
    }

    #[test]
    fn recording_mode_prefers_cached_window_dc() {
        let (attempts, count) = build_window_capture_attempts(CaptureMode::Continuous, None);
        assert_eq!(count, 4);
        assert_eq!(attempts[0], WindowCapturePath::WindowDcBitBlt);
        assert_eq!(
            attempts[1],
            WindowCapturePath::PrintWindow(PRINT_WINDOW_RENDER_FULL)
        );
    }

    #[test]
    fn preferred_path_is_front_loaded_without_duplicates() {
        let preferred = WindowCapturePath::PrintWindow(PRINT_WINDOW_DEFAULT);
        let (attempts, count) =
            build_window_capture_attempts(CaptureMode::Continuous, Some(preferred));
        assert_eq!(count, 4);
        assert_eq!(attempts[0], preferred);
        for idx in 0..count {
            assert!(!attempts[..idx].contains(&attempts[idx]));
        }
    }

    #[test]
    fn screenshot_mode_ignores_cached_preferred_path() {
        let preferred = WindowCapturePath::WindowDcBitBlt;
        let (attempts, count) =
            build_window_capture_attempts(CaptureMode::Snapshot, Some(preferred));
        assert_eq!(count, 4);
        assert_eq!(attempts[0], preferred);
        for idx in 0..count {
            assert!(!attempts[..idx].contains(&attempts[idx]));
        }
    }

    #[test]
    fn dirty_row_run_builder_merges_adjacent_rows() {
        let flags = [0u8, 1, 1, 0, 1, 0, 1, 1, 1, 0];
        let mut runs = Vec::new();
        build_dirty_row_runs_from_flags(&flags, &mut runs);

        assert_eq!(runs.len(), 3);
        assert_eq!(runs[0].start_row, 1);
        assert_eq!(runs[0].row_count, 2);
        assert_eq!(runs[1].start_row, 4);
        assert_eq!(runs[1].row_count, 1);
        assert_eq!(runs[2].start_row, 6);
        assert_eq!(runs[2].row_count, 3);
    }

    #[test]
    fn dirty_row_run_builder_handles_empty_and_trailing_runs() {
        let mut runs = Vec::new();
        build_dirty_row_runs_from_flags(&[], &mut runs);
        assert!(runs.is_empty());

        let flags = [1u8, 1, 1];
        build_dirty_row_runs_from_flags(&flags, &mut runs);
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].start_row, 0);
        assert_eq!(runs[0].row_count, 3);
    }

    #[test]
    fn dirty_span_run_builder_merges_only_matching_spans() {
        let spans = [
            DirtyRowSpan {
                start_col: 4,
                width: 6,
            },
            DirtyRowSpan {
                start_col: 4,
                width: 6,
            },
            DirtyRowSpan::default(),
            DirtyRowSpan {
                start_col: 2,
                width: 5,
            },
            DirtyRowSpan {
                start_col: 2,
                width: 5,
            },
            DirtyRowSpan {
                start_col: 3,
                width: 5,
            },
        ];
        let mut runs = Vec::new();
        build_dirty_span_runs_from_spans(&spans, &mut runs);

        assert_eq!(runs.len(), 3);
        assert_eq!(runs[0].start_row, 0);
        assert_eq!(runs[0].row_count, 2);
        assert_eq!(runs[0].start_col, 4);
        assert_eq!(runs[0].width, 6);

        assert_eq!(runs[1].start_row, 3);
        assert_eq!(runs[1].row_count, 2);
        assert_eq!(runs[1].start_col, 2);
        assert_eq!(runs[1].width, 5);

        assert_eq!(runs[2].start_row, 5);
        assert_eq!(runs[2].row_count, 1);
        assert_eq!(runs[2].start_col, 3);
        assert_eq!(runs[2].width, 5);
    }

    #[test]
    fn parallel_span_mode_recommendation_prefers_compare_for_sparse_damage() {
        assert_eq!(
            recommend_parallel_span_scan_mode(0, 0, 2560, 1440),
            ParallelSpanScanMode::CompareThenDiff
        );
        assert_eq!(
            recommend_parallel_span_scan_mode(720, 69_120, 2560, 1440),
            ParallelSpanScanMode::CompareThenDiff
        );
    }

    #[test]
    fn parallel_span_mode_recommendation_prefers_single_for_dense_damage() {
        assert_eq!(
            recommend_parallel_span_scan_mode(1300, 900_000, 2560, 1440),
            ParallelSpanScanMode::SingleScanDiff
        );
        assert_eq!(
            recommend_parallel_span_scan_mode(1000, 1_200_000, 2560, 1440),
            ParallelSpanScanMode::SingleScanDiff
        );
    }

    #[test]
    fn parallel_span_mode_selection_prefers_duplicate_hint_over_mode_history() {
        assert_eq!(
            select_parallel_span_scan_mode(
                std::ptr::null(),
                0,
                std::ptr::null(),
                0,
                0,
                0,
                row_compare_kernel(),
                true,
                Some(ParallelSpanScanMode::SingleScanDiff),
            ),
            ParallelSpanScanMode::CompareThenDiff
        );
    }

    #[test]
    fn row_compare_scalar_detects_changes() {
        let mut lhs = vec![0u8; 257];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(3);
        }
        let mut rhs = lhs.clone();

        assert!(unsafe { row_equal_scalar(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) });

        rhs[128] ^= 0x7F;
        assert!(!unsafe { row_equal_scalar(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) });
    }

    #[test]
    fn row_diff_bounds_scalar_returns_trimmed_change_window() {
        let mut lhs = vec![0u8; 96];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(9).wrapping_add(1);
        }
        let mut rhs = lhs.clone();
        rhs[17] ^= 0x44;
        rhs[73] ^= 0x12;

        let bounds = unsafe { row_diff_bounds_scalar(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) };
        assert_eq!(bounds, Some((17, 74)));
    }

    #[test]
    fn row_diff_span_pixels_expands_to_full_pixels() {
        let width = 12usize;
        let mut lhs = vec![0u8; width * BGRA_BYTES_PER_PIXEL];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(7).wrapping_add(3);
        }
        let mut rhs = lhs.clone();

        rhs[3 * BGRA_BYTES_PER_PIXEL + 1] ^= 0x1F;
        rhs[9 * BGRA_BYTES_PER_PIXEL + 2] ^= 0x2A;

        let span =
            unsafe { row_diff_span_pixels(lhs.as_ptr(), rhs.as_ptr(), lhs.len(), width).unwrap() };
        assert_eq!(span, (3, 10));
    }

    #[test]
    fn selected_row_diff_kernel_matches_scalar() {
        let mut lhs = vec![0u8; 4096];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(19).wrapping_add(7);
        }
        let mut rhs = lhs.clone();

        let kernel = row_diff_bounds_kernel();
        assert_eq!(
            unsafe { kernel(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) },
            None
        );

        rhs[129] ^= 0xA5;
        rhs[3011] ^= 0x6C;
        let scalar = unsafe { row_diff_bounds_scalar(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) };
        let selected = unsafe { kernel(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) };
        assert_eq!(selected, scalar);
    }

    #[test]
    fn selected_row_compare_kernel_matches_scalar() {
        let mut lhs = vec![0u8; 4096];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(11);
        }
        let mut rhs = lhs.clone();
        rhs[2048] ^= 0x55;

        let kernel = row_compare_kernel();
        let scalar_equal = unsafe { row_equal_scalar(lhs.as_ptr(), lhs.as_ptr(), lhs.len()) };
        let kernel_equal = unsafe { kernel(lhs.as_ptr(), lhs.as_ptr(), lhs.len()) };
        assert_eq!(kernel_equal, scalar_equal);

        let scalar_diff = unsafe { row_equal_scalar(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) };
        let kernel_diff = unsafe { kernel(lhs.as_ptr(), rhs.as_ptr(), lhs.len()) };
        assert_eq!(kernel_diff, scalar_diff);
    }

    #[test]
    fn too_dirty_probe_detects_highly_dynamic_surface() {
        let width = 64usize;
        let height = 96usize;
        let row_bytes = width * 4;
        let mut current = vec![0u8; row_bytes * height];
        let history = current.clone();

        for (idx, byte) in current.iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_add(19);
        }

        assert!(unsafe {
            incremental_too_dirty_probe(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                height,
                row_equal_scalar,
            )
        });
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn simd_row_diff_variants_match_scalar() {
        let mut lhs = vec![0u8; 12288];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(29).wrapping_add(5);
        }
        let mut rhs = lhs.clone();

        let lengths = [17usize, 33, 97, 256, 777, 2049, 4097, lhs.len()];
        for &len in &lengths {
            let scalar = unsafe { row_diff_bounds_scalar(lhs.as_ptr(), rhs.as_ptr(), len) };
            if std::arch::is_x86_feature_detected!("sse2") {
                assert_eq!(
                    unsafe { row_diff_bounds_sse2(lhs.as_ptr(), rhs.as_ptr(), len) },
                    scalar
                );
            }
            if std::arch::is_x86_feature_detected!("avx2") {
                assert_eq!(
                    unsafe { row_diff_bounds_avx2(lhs.as_ptr(), rhs.as_ptr(), len) },
                    scalar
                );
            }
        }

        rhs[11] ^= 0x31;
        rhs[2048] ^= 0x22;
        rhs[9037] ^= 0x6B;
        for &len in &lengths {
            let scalar = unsafe { row_diff_bounds_scalar(lhs.as_ptr(), rhs.as_ptr(), len) };
            if std::arch::is_x86_feature_detected!("sse2") {
                assert_eq!(
                    unsafe { row_diff_bounds_sse2(lhs.as_ptr(), rhs.as_ptr(), len) },
                    scalar
                );
            }
            if std::arch::is_x86_feature_detected!("avx2") {
                assert_eq!(
                    unsafe { row_diff_bounds_avx2(lhs.as_ptr(), rhs.as_ptr(), len) },
                    scalar
                );
            }
        }
    }

    #[test]
    fn too_dirty_probe_ignores_sparse_row_updates() {
        let width = 96usize;
        let height = 120usize;
        let row_bytes = width * 4;
        let mut current = vec![0u8; row_bytes * height];
        for (idx, value) in current.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(5).wrapping_add(3);
        }
        let mut history = current.clone();

        let sparse_rows = [4usize, 19, 37, 74, 101];
        for &row in &sparse_rows {
            let start = row * row_bytes;
            let end = start + row_bytes;
            for byte in &mut history[start..end] {
                *byte ^= 0x5A;
            }
        }

        assert!(!unsafe {
            incremental_too_dirty_probe(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                height,
                row_equal_scalar,
            )
        });
    }

    #[test]
    fn too_dirty_probe_respects_threshold_boundary() {
        let width = 48usize;
        let height = 24usize;
        let row_bytes = width * 4;
        let mut current = vec![0u8; row_bytes * height];
        let mut history = vec![0u8; row_bytes * height];

        for row in 0..19usize {
            let start = row * row_bytes;
            let end = start + row_bytes;
            for byte in &mut current[start..end] {
                *byte = 0x33;
            }
        }
        assert!(!unsafe {
            incremental_too_dirty_probe(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                height,
                row_equal_scalar,
            )
        });

        let row = 19usize;
        let start = row * row_bytes;
        let end = start + row_bytes;
        for byte in &mut history[start..end] {
            *byte = 0x77;
        }
        assert!(unsafe {
            incremental_too_dirty_probe(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                height,
                row_equal_scalar,
            )
        });
    }

    #[cfg(target_arch = "x86_64")]
    #[test]
    fn simd_row_compare_variants_match_baseline_kernels() {
        let mut lhs = vec![0u8; 8192];
        for (idx, value) in lhs.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(37).wrapping_add(11);
        }
        let mut rhs = lhs.clone();

        let test_lengths = [31usize, 64, 127, 255, 512, 1023, 4097, lhs.len()];
        for &len in &test_lengths {
            assert_eq!(
                unsafe { row_equal_sse2_unrolled_baseline(lhs.as_ptr(), rhs.as_ptr(), len) },
                unsafe { row_equal_sse2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
            );
            assert_eq!(
                unsafe { row_equal_sse2_unrolled(lhs.as_ptr(), rhs.as_ptr(), len) },
                unsafe { row_equal_sse2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
            );
        }

        rhs[2027] ^= 0x5A;
        for &len in &test_lengths {
            assert_eq!(
                unsafe { row_equal_sse2_unrolled_baseline(lhs.as_ptr(), rhs.as_ptr(), len) },
                unsafe { row_equal_sse2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
            );
            assert_eq!(
                unsafe { row_equal_sse2_unrolled(lhs.as_ptr(), rhs.as_ptr(), len) },
                unsafe { row_equal_sse2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
            );
        }

        if std::arch::is_x86_feature_detected!("avx2") {
            rhs.copy_from_slice(&lhs);
            for &len in &test_lengths {
                assert_eq!(
                    unsafe { row_equal_avx2_unrolled_baseline(lhs.as_ptr(), rhs.as_ptr(), len) },
                    unsafe { row_equal_avx2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
                );
                assert_eq!(
                    unsafe { row_equal_avx2_unrolled(lhs.as_ptr(), rhs.as_ptr(), len) },
                    unsafe { row_equal_avx2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
                );
            }

            rhs[6015] ^= 0x31;
            for &len in &test_lengths {
                assert_eq!(
                    unsafe { row_equal_avx2_unrolled_baseline(lhs.as_ptr(), rhs.as_ptr(), len) },
                    unsafe { row_equal_avx2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
                );
                assert_eq!(
                    unsafe { row_equal_avx2_unrolled(lhs.as_ptr(), rhs.as_ptr(), len) },
                    unsafe { row_equal_avx2_baseline(lhs.as_ptr(), rhs.as_ptr(), len) }
                );
            }
        }
    }

    unsafe fn scan_dirty_pixels_baseline(
        src_base: *const u8,
        src_stride: usize,
        history_base: *const u8,
        history_stride: usize,
        row_bytes: usize,
        width: usize,
        height: usize,
        compare_row: RowCompareKernel,
    ) -> usize {
        let mut dirty_pixels = 0usize;
        let mut src_row = src_base;
        let mut history_row = history_base;
        for _ in 0..height {
            let changed = unsafe { !compare_row(src_row, history_row, row_bytes) };
            if changed {
                let (start_col, end_col) =
                    unsafe { row_diff_span_pixels(src_row, history_row, row_bytes, width) }
                        .unwrap_or((0, width));
                dirty_pixels = dirty_pixels.saturating_add(end_col.saturating_sub(start_col));
            }

            unsafe {
                src_row = src_row.add(src_stride);
                history_row = history_row.add(history_stride);
            }
        }
        dirty_pixels
    }

    unsafe fn scan_dirty_pixels_single_scan(
        src_base: *const u8,
        src_stride: usize,
        history_base: *const u8,
        history_stride: usize,
        row_bytes: usize,
        width: usize,
        height: usize,
    ) -> usize {
        let mut dirty_pixels = 0usize;
        let mut src_row = src_base;
        let mut history_row = history_base;
        for _ in 0..height {
            if let Some((start_col, end_col)) =
                unsafe { row_diff_span_pixels(src_row, history_row, row_bytes, width) }
            {
                dirty_pixels = dirty_pixels.saturating_add(end_col.saturating_sub(start_col));
            }
            unsafe {
                src_row = src_row.add(src_stride);
                history_row = history_row.add(history_stride);
            }
        }
        dirty_pixels
    }

    fn build_sparse_damage_variant(
        baseline: &[u8],
        width: usize,
        height: usize,
        seed: usize,
    ) -> Vec<u8> {
        if width <= 1 || height == 0 {
            return baseline.to_vec();
        }
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let mut out = baseline.to_vec();
        let span_width = 96usize.min(width.saturating_sub(1)).max(1);
        let span_width_bytes = span_width * BGRA_BYTES_PER_PIXEL;
        for row in (0..height).step_by(2) {
            let start_col = (row * 37 + seed * 53) % (width - span_width);
            let start = row
                .checked_mul(row_bytes)
                .and_then(|offset| offset.checked_add(start_col * BGRA_BYTES_PER_PIXEL))
                .unwrap();
            let end = start + span_width_bytes;
            for (idx, byte) in out[start..end].iter_mut().enumerate() {
                *byte ^= ((idx + row + seed * 17) as u8).wrapping_mul(19);
            }
        }
        out
    }

    unsafe fn incremental_step_parallel_rows_baseline(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        _height: usize,
        compare_row: RowCompareKernel,
        flags: &mut [u8],
        row_runs: &mut Vec<DirtyRowRun>,
    ) -> usize {
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let dirty_rows = scan_dirty_row_flags_parallel(
            flags,
            src_base,
            src_stride,
            history_base,
            history_stride,
            row_bytes,
            compare_row,
            true,
        );
        build_dirty_row_runs_from_flags(flags, row_runs);

        let converter = bgra_row_converter();
        for run in row_runs.iter() {
            let src_ptr = unsafe { src_base.add(run.start_row * src_stride) };
            let dst_ptr = unsafe { dst_base.add(run.start_row * dst_stride) };
            unsafe {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_ptr,
                    src_stride,
                    dst_ptr,
                    dst_stride,
                    width,
                    run.row_count,
                );
            }
        }

        dirty_rows.saturating_mul(width)
    }

    unsafe fn incremental_step_parallel_spans_baseline(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        height: usize,
        compare_row: RowCompareKernel,
        flags: &mut [u8],
        row_spans: &mut [DirtyRowSpan],
        span_runs: &mut Vec<DirtySpanRun>,
    ) -> usize {
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let dirty_rows = scan_dirty_row_flags_parallel(
            flags,
            src_base,
            src_stride,
            history_base,
            history_stride,
            row_bytes,
            compare_row,
            false,
        );
        if dirty_rows == 0 {
            span_runs.clear();
            row_spans.fill(DirtyRowSpan::default());
            return 0;
        }

        row_spans.fill(DirtyRowSpan::default());
        let mut dirty_pixels = 0usize;
        let mut src_row = src_base;
        let mut history_row = history_base;
        for (row, &flag) in flags.iter().enumerate().take(height) {
            if flag != 0 {
                let (start_col, end_col) = unsafe {
                    row_diff_span_pixels(src_row, history_row.cast_const(), row_bytes, width)
                }
                .unwrap_or((0, width));
                let span_width = end_col.saturating_sub(start_col);
                row_spans[row] = DirtyRowSpan {
                    start_col: start_col as u32,
                    width: span_width as u32,
                };
                dirty_pixels = dirty_pixels.saturating_add(span_width);

                let start_byte = start_col * BGRA_BYTES_PER_PIXEL;
                let copy_bytes = span_width * BGRA_BYTES_PER_PIXEL;
                if copy_bytes > 0 {
                    unsafe {
                        std::ptr::copy_nonoverlapping(
                            src_row.add(start_byte),
                            history_row.add(start_byte),
                            copy_bytes,
                        );
                    }
                }
            }

            unsafe {
                src_row = src_row.add(src_stride);
                history_row = history_row.add(history_stride);
            }
        }

        build_dirty_span_runs_from_spans(row_spans, span_runs);
        let converter = bgra_row_converter();
        for run in span_runs.iter() {
            let row_offset = run.start_row * src_stride + run.start_col * BGRA_BYTES_PER_PIXEL;
            let src_ptr = unsafe { src_base.add(row_offset) };
            let dst_ptr = unsafe {
                dst_base.add(run.start_row * dst_stride + run.start_col * BGRA_BYTES_PER_PIXEL)
            };
            unsafe {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_ptr,
                    src_stride,
                    dst_ptr,
                    dst_stride,
                    run.width,
                    run.row_count,
                );
            }
        }

        dirty_pixels
    }

    unsafe fn incremental_step_parallel_spans_with_mode(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        height: usize,
        row_spans: &mut [DirtyRowSpan],
        span_runs: &mut Vec<DirtySpanRun>,
        scan_mode: ParallelSpanScanMode,
    ) -> (usize, usize) {
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let spans = &mut row_spans[..height];
        spans.fill(DirtyRowSpan::default());
        let (dirty_rows, dirty_pixels) = scan_dirty_row_spans_parallel(
            spans,
            src_base,
            src_stride,
            history_base,
            history_stride,
            row_bytes,
            width,
            row_compare_kernel(),
            row_diff_bounds_kernel(),
            true,
            scan_mode,
        );
        if dirty_rows == 0 {
            span_runs.clear();
            return (0, 0);
        }

        build_dirty_span_runs_from_spans(spans, span_runs);
        let converter = bgra_row_converter();
        for run in span_runs.iter() {
            let row_offset = run.start_row * src_stride + run.start_col * BGRA_BYTES_PER_PIXEL;
            let src_ptr = unsafe { src_base.add(row_offset) };
            let dst_ptr = unsafe {
                dst_base.add(run.start_row * dst_stride + run.start_col * BGRA_BYTES_PER_PIXEL)
            };
            unsafe {
                converter.convert_rows_maybe_parallel_unchecked(
                    src_ptr,
                    src_stride,
                    dst_ptr,
                    dst_stride,
                    run.width,
                    run.row_count,
                );
            }
        }

        (dirty_rows, dirty_pixels)
    }

    unsafe fn incremental_step_parallel_spans_compare_then_diff(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        height: usize,
        row_spans: &mut [DirtyRowSpan],
        span_runs: &mut Vec<DirtySpanRun>,
    ) -> usize {
        unsafe {
            incremental_step_parallel_spans_with_mode(
                src_base,
                src_stride,
                history_base,
                history_stride,
                dst_base,
                dst_stride,
                width,
                height,
                row_spans,
                span_runs,
                ParallelSpanScanMode::CompareThenDiff,
            )
            .1
        }
    }

    unsafe fn incremental_step_parallel_spans_single_pass(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        height: usize,
        row_spans: &mut [DirtyRowSpan],
        span_runs: &mut Vec<DirtySpanRun>,
    ) -> usize {
        unsafe {
            incremental_step_parallel_spans_with_mode(
                src_base,
                src_stride,
                history_base,
                history_stride,
                dst_base,
                dst_stride,
                width,
                height,
                row_spans,
                span_runs,
                ParallelSpanScanMode::SingleScanDiff,
            )
            .1
        }
    }

    unsafe fn incremental_step_parallel_spans_auto(
        src_base: *const u8,
        src_stride: usize,
        history_base: *mut u8,
        history_stride: usize,
        dst_base: *mut u8,
        dst_stride: usize,
        width: usize,
        height: usize,
        duplicate_probe_hint: bool,
        mode_hint: &mut Option<ParallelSpanScanMode>,
        row_spans: &mut [DirtyRowSpan],
        span_runs: &mut Vec<DirtySpanRun>,
    ) -> usize {
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let scan_mode = select_parallel_span_scan_mode(
            src_base,
            src_stride,
            history_base.cast_const(),
            history_stride,
            row_bytes,
            height,
            row_compare_kernel(),
            duplicate_probe_hint,
            *mode_hint,
        );
        let (dirty_rows, dirty_pixels) = unsafe {
            incremental_step_parallel_spans_with_mode(
                src_base,
                src_stride,
                history_base,
                history_stride,
                dst_base,
                dst_stride,
                width,
                height,
                row_spans,
                span_runs,
                scan_mode,
            )
        };
        *mode_hint = Some(recommend_parallel_span_scan_mode(
            dirty_rows,
            dirty_pixels,
            width,
            height,
        ));
        dirty_pixels
    }

    #[test]
    fn parallel_span_scan_matches_parallel_row_output_sparse_damage() {
        let width = 640usize;
        let height = 360usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;

        let mut baseline = vec![0u8; total_bytes];
        for (idx, value) in baseline.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(13).wrapping_add(7);
        }
        let frame_a = build_sparse_damage_variant(&baseline, width, height, 1);
        let frame_b = build_sparse_damage_variant(&baseline, width, height, 5);
        let frames = [&frame_a[..], &frame_b[..]];

        let mut history_row = frame_a.clone();
        let mut history_span = frame_a.clone();
        let mut dst_row = vec![0u8; total_bytes];
        let mut dst_span = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                dst_row.as_mut_ptr(),
                width * height,
            );
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                dst_span.as_mut_ptr(),
                width * height,
            );
        }

        let compare = row_compare_kernel();
        let mut flags_row = vec![0u8; height];
        let mut row_runs = Vec::new();
        let mut row_spans = vec![DirtyRowSpan::default(); height];
        let mut span_runs = Vec::new();
        let mut expected = vec![0u8; total_bytes];
        let mut phase = 1usize;
        for _ in 0..10usize {
            let src = frames[phase];
            unsafe {
                incremental_step_parallel_rows_baseline(
                    src.as_ptr(),
                    row_bytes,
                    history_row.as_mut_ptr(),
                    row_bytes,
                    dst_row.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    compare,
                    &mut flags_row,
                    &mut row_runs,
                );
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    history_span.as_mut_ptr(),
                    row_bytes,
                    dst_span.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut row_spans,
                    &mut span_runs,
                );
                convert::convert_bgra_to_rgba_unchecked(
                    src.as_ptr(),
                    expected.as_mut_ptr(),
                    width * height,
                );
            }
            assert_eq!(dst_row, expected);
            assert_eq!(dst_span, expected);
            phase ^= 1;
        }
    }

    #[test]
    fn parallel_span_single_scan_matches_compare_then_diff_sparse_damage() {
        let width = 1280usize;
        let height = 720usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;
        let pixel_count = width * height;

        let mut baseline = vec![0u8; total_bytes];
        for (idx, value) in baseline.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(11).wrapping_add(41);
        }
        let frame_a = build_sparse_damage_variant(&baseline, width, height, 2);
        let frame_b = build_sparse_damage_variant(&baseline, width, height, 8);
        let frames = [&frame_a[..], &frame_b[..]];

        let mut history_compare = frame_a.clone();
        let mut history_single = frame_a.clone();
        let mut dst_compare = vec![0u8; total_bytes];
        let mut dst_single = vec![0u8; total_bytes];
        let mut expected = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                dst_compare.as_mut_ptr(),
                pixel_count,
            );
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                dst_single.as_mut_ptr(),
                pixel_count,
            );
        }

        let mut spans_compare = vec![DirtyRowSpan::default(); height];
        let mut spans_single = vec![DirtyRowSpan::default(); height];
        let mut runs_compare = Vec::new();
        let mut runs_single = Vec::new();

        let mut phase = 1usize;
        for _ in 0..12usize {
            let src = frames[phase];
            let compare_dirty_pixels = unsafe {
                incremental_step_parallel_spans_compare_then_diff(
                    src.as_ptr(),
                    row_bytes,
                    history_compare.as_mut_ptr(),
                    row_bytes,
                    dst_compare.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut spans_compare,
                    &mut runs_compare,
                )
            };
            let single_dirty_pixels = unsafe {
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    history_single.as_mut_ptr(),
                    row_bytes,
                    dst_single.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut spans_single,
                    &mut runs_single,
                )
            };

            unsafe {
                convert::convert_bgra_to_rgba_unchecked(
                    src.as_ptr(),
                    expected.as_mut_ptr(),
                    pixel_count,
                );
            }

            assert_eq!(compare_dirty_pixels, single_dirty_pixels);
            assert_eq!(dst_compare, dst_single);
            assert_eq!(dst_single, expected);
            assert_eq!(history_compare, history_single);
            phase ^= 1;
        }
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_parallel_span_auto_vs_compare_then_diff_sparse_damage() {
        use std::hint::black_box;

        let width = 2560usize;
        let height = 1440usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;
        let pixel_count = width * height;

        let mut baseline = vec![0u8; total_bytes];
        for (idx, value) in baseline.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(37).wrapping_add(13);
        }
        let frame_a = build_sparse_damage_variant(&baseline, width, height, 4);
        let frame_b = build_sparse_damage_variant(&baseline, width, height, 11);
        let frames = [&frame_a[..], &frame_b[..]];

        let warmup_iters = 20usize;
        let measure_iters = 140usize;

        let mut compare_history = frame_a.clone();
        let mut compare_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                compare_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut compare_spans = vec![DirtyRowSpan::default(); height];
        let mut compare_runs = Vec::new();

        let mut sink = 0usize;
        let mut phase = 1usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_compare_then_diff(
                    src.as_ptr(),
                    row_bytes,
                    compare_history.as_mut_ptr(),
                    row_bytes,
                    compare_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut compare_spans,
                    &mut compare_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let compare_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_compare_then_diff(
                    src.as_ptr(),
                    row_bytes,
                    compare_history.as_mut_ptr(),
                    row_bytes,
                    compare_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut compare_spans,
                    &mut compare_runs,
                )
            };
            phase ^= 1;
        }
        let compare_elapsed = compare_start.elapsed();

        let mut auto_history = frame_a.clone();
        let mut auto_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                auto_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut auto_spans = vec![DirtyRowSpan::default(); height];
        let mut auto_runs = Vec::new();
        let mut auto_mode_hint = None;
        phase = 1usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_auto(
                    src.as_ptr(),
                    row_bytes,
                    auto_history.as_mut_ptr(),
                    row_bytes,
                    auto_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    false,
                    &mut auto_mode_hint,
                    &mut auto_spans,
                    &mut auto_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let auto_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_auto(
                    src.as_ptr(),
                    row_bytes,
                    auto_history.as_mut_ptr(),
                    row_bytes,
                    auto_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    false,
                    &mut auto_mode_hint,
                    &mut auto_spans,
                    &mut auto_runs,
                )
            };
            phase ^= 1;
        }
        let auto_elapsed = auto_start.elapsed();
        black_box(sink);

        let compare_ms = compare_elapsed.as_secs_f64() * 1000.0;
        let auto_ms = auto_elapsed.as_secs_f64() * 1000.0;
        let improvement_pct = if compare_ms > 0.0 {
            ((compare_ms - auto_ms) / compare_ms) * 100.0
        } else {
            0.0
        };

        println!(
            "gdi parallel span auto benchmark: compare_then_diff={compare_ms:.3} ms auto={auto_ms:.3} ms improvement={improvement_pct:.2}%"
        );

        assert!(
            auto_ms <= compare_ms * 1.12,
            "parallel auto span path regressed: compare_then_diff={compare_ms:.3}ms auto={auto_ms:.3}ms ({improvement_pct:.2}% improvement)"
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_parallel_span_auto_vs_compare_then_diff_duplicate_surface() {
        use std::hint::black_box;

        let width = 2560usize;
        let height = 1440usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;
        let pixel_count = width * height;

        let mut frame = vec![0u8; total_bytes];
        for (idx, value) in frame.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(17).wrapping_add(61);
        }

        let warmup_iters = 40usize;
        let measure_iters = 1200usize;

        let mut compare_history = frame.clone();
        let mut compare_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame.as_ptr(),
                compare_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut compare_spans = vec![DirtyRowSpan::default(); height];
        let mut compare_runs = Vec::new();
        let mut sink = 0usize;
        for _ in 0..warmup_iters {
            sink ^= unsafe {
                incremental_step_parallel_spans_compare_then_diff(
                    frame.as_ptr(),
                    row_bytes,
                    compare_history.as_mut_ptr(),
                    row_bytes,
                    compare_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut compare_spans,
                    &mut compare_runs,
                )
            };
        }

        let compare_start = Instant::now();
        for _ in 0..measure_iters {
            sink ^= unsafe {
                incremental_step_parallel_spans_compare_then_diff(
                    frame.as_ptr(),
                    row_bytes,
                    compare_history.as_mut_ptr(),
                    row_bytes,
                    compare_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut compare_spans,
                    &mut compare_runs,
                )
            };
        }
        let compare_elapsed = compare_start.elapsed();

        let mut auto_history = frame.clone();
        let mut auto_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame.as_ptr(),
                auto_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut auto_spans = vec![DirtyRowSpan::default(); height];
        let mut auto_runs = Vec::new();
        let mut auto_mode_hint = None;
        for _ in 0..warmup_iters {
            sink ^= unsafe {
                incremental_step_parallel_spans_auto(
                    frame.as_ptr(),
                    row_bytes,
                    auto_history.as_mut_ptr(),
                    row_bytes,
                    auto_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    false,
                    &mut auto_mode_hint,
                    &mut auto_spans,
                    &mut auto_runs,
                )
            };
        }

        let auto_start = Instant::now();
        for _ in 0..measure_iters {
            sink ^= unsafe {
                incremental_step_parallel_spans_auto(
                    frame.as_ptr(),
                    row_bytes,
                    auto_history.as_mut_ptr(),
                    row_bytes,
                    auto_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    false,
                    &mut auto_mode_hint,
                    &mut auto_spans,
                    &mut auto_runs,
                )
            };
        }
        let auto_elapsed = auto_start.elapsed();
        black_box(sink);

        let compare_ms = compare_elapsed.as_secs_f64() * 1000.0;
        let auto_ms = auto_elapsed.as_secs_f64() * 1000.0;
        let delta_pct = if compare_ms > 0.0 {
            ((auto_ms - compare_ms) / compare_ms) * 100.0
        } else {
            0.0
        };

        println!(
            "gdi parallel span duplicate benchmark: compare_then_diff={compare_ms:.3} ms auto={auto_ms:.3} ms delta={delta_pct:.2}%"
        );

        assert!(
            auto_ms <= compare_ms * 1.20,
            "parallel auto span duplicate path regressed: compare_then_diff={compare_ms:.3}ms auto={auto_ms:.3}ms ({delta_pct:.2}% delta)"
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_parallel_span_scan_vs_parallel_row_sparse_damage() {
        use std::hint::black_box;

        let width = 2560usize;
        let height = 1440usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;
        let pixel_count = width * height;
        let compare = row_compare_kernel();

        let mut baseline = vec![0u8; total_bytes];
        for (idx, value) in baseline.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(23).wrapping_add(19);
        }
        let frame_a = build_sparse_damage_variant(&baseline, width, height, 2);
        let frame_b = build_sparse_damage_variant(&baseline, width, height, 7);
        let frames = [&frame_a[..], &frame_b[..]];

        let warmup_iters = 20usize;
        let measure_iters = 140usize;

        let mut baseline_history = frame_a.clone();
        let mut baseline_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                baseline_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut baseline_flags = vec![0u8; height];
        let mut baseline_runs = Vec::new();
        let mut phase = 1usize;
        let mut sink = 0usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_rows_baseline(
                    src.as_ptr(),
                    row_bytes,
                    baseline_history.as_mut_ptr(),
                    row_bytes,
                    baseline_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    compare,
                    &mut baseline_flags,
                    &mut baseline_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let baseline_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_rows_baseline(
                    src.as_ptr(),
                    row_bytes,
                    baseline_history.as_mut_ptr(),
                    row_bytes,
                    baseline_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    compare,
                    &mut baseline_flags,
                    &mut baseline_runs,
                )
            };
            phase ^= 1;
        }
        let baseline_elapsed = baseline_start.elapsed();

        let mut span_history = frame_a.clone();
        let mut span_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                span_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut span_spans = vec![DirtyRowSpan::default(); height];
        let mut span_runs = Vec::new();
        phase = 1usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    span_history.as_mut_ptr(),
                    row_bytes,
                    span_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut span_spans,
                    &mut span_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let span_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    span_history.as_mut_ptr(),
                    row_bytes,
                    span_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut span_spans,
                    &mut span_runs,
                )
            };
            phase ^= 1;
        }
        let span_elapsed = span_start.elapsed();
        black_box(sink);

        let baseline_ms = baseline_elapsed.as_secs_f64() * 1000.0;
        let span_ms = span_elapsed.as_secs_f64() * 1000.0;
        let improvement_pct = if baseline_ms > 0.0 {
            ((baseline_ms - span_ms) / baseline_ms) * 100.0
        } else {
            0.0
        };

        println!(
            "gdi parallel span benchmark: baseline={baseline_ms:.3} ms span={span_ms:.3} ms improvement={improvement_pct:.2}%"
        );

        assert!(
            span_ms <= baseline_ms * 1.03,
            "parallel span path regressed: baseline={baseline_ms:.3}ms span={span_ms:.3}ms ({improvement_pct:.2}% improvement)"
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_parallel_span_single_pass_vs_baseline_two_pass_sparse_damage() {
        use std::hint::black_box;

        let width = 2560usize;
        let height = 1440usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;
        let pixel_count = width * height;
        let compare = row_compare_kernel();

        let mut baseline = vec![0u8; total_bytes];
        for (idx, value) in baseline.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(17).wrapping_add(29);
        }
        let frame_a = build_sparse_damage_variant(&baseline, width, height, 3);
        let frame_b = build_sparse_damage_variant(&baseline, width, height, 9);
        let frames = [&frame_a[..], &frame_b[..]];

        let warmup_iters = 20usize;
        let measure_iters = 140usize;
        let mut sink = 0usize;

        let mut baseline_history = frame_a.clone();
        let mut baseline_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                baseline_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut baseline_flags = vec![0u8; height];
        let mut baseline_spans = vec![DirtyRowSpan::default(); height];
        let mut baseline_runs = Vec::new();
        let mut phase = 1usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_baseline(
                    src.as_ptr(),
                    row_bytes,
                    baseline_history.as_mut_ptr(),
                    row_bytes,
                    baseline_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    compare,
                    &mut baseline_flags,
                    &mut baseline_spans,
                    &mut baseline_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let baseline_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_baseline(
                    src.as_ptr(),
                    row_bytes,
                    baseline_history.as_mut_ptr(),
                    row_bytes,
                    baseline_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    compare,
                    &mut baseline_flags,
                    &mut baseline_spans,
                    &mut baseline_runs,
                )
            };
            phase ^= 1;
        }
        let baseline_elapsed = baseline_start.elapsed();

        let mut single_history = frame_a.clone();
        let mut single_dst = vec![0u8; total_bytes];
        unsafe {
            convert::convert_bgra_to_rgba_unchecked(
                frame_a.as_ptr(),
                single_dst.as_mut_ptr(),
                pixel_count,
            );
        }
        let mut single_spans = vec![DirtyRowSpan::default(); height];
        let mut single_runs = Vec::new();
        phase = 1usize;
        for _ in 0..warmup_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    single_history.as_mut_ptr(),
                    row_bytes,
                    single_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut single_spans,
                    &mut single_runs,
                )
            };
            phase ^= 1;
        }

        phase = 1usize;
        let single_start = Instant::now();
        for _ in 0..measure_iters {
            let src = frames[phase];
            sink ^= unsafe {
                incremental_step_parallel_spans_single_pass(
                    src.as_ptr(),
                    row_bytes,
                    single_history.as_mut_ptr(),
                    row_bytes,
                    single_dst.as_mut_ptr(),
                    row_bytes,
                    width,
                    height,
                    &mut single_spans,
                    &mut single_runs,
                )
            };
            phase ^= 1;
        }
        let single_elapsed = single_start.elapsed();
        black_box(sink);

        let baseline_ms = baseline_elapsed.as_secs_f64() * 1000.0;
        let single_ms = single_elapsed.as_secs_f64() * 1000.0;
        let improvement_pct = if baseline_ms > 0.0 {
            ((baseline_ms - single_ms) / baseline_ms) * 100.0
        } else {
            0.0
        };

        println!(
            "gdi parallel span single-pass benchmark: two_pass={baseline_ms:.3} ms single_pass={single_ms:.3} ms improvement={improvement_pct:.2}%"
        );

        assert!(
            single_ms <= baseline_ms * 1.03,
            "single-pass parallel span path regressed: two_pass={baseline_ms:.3}ms single_pass={single_ms:.3}ms ({improvement_pct:.2}% improvement)"
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_span_single_scan_vs_baseline_sparse_damage() {
        use std::hint::black_box;

        let width = 1600usize;
        let height = 900usize;
        let row_bytes = width * BGRA_BYTES_PER_PIXEL;
        let total_bytes = row_bytes * height;

        let mut current = vec![0u8; total_bytes];
        for (idx, value) in current.iter_mut().enumerate() {
            *value = (idx as u8).wrapping_mul(31).wrapping_add(17);
        }
        let history = current.clone();

        let span_width_pixels = 96usize;
        let span_width_bytes = span_width_pixels * BGRA_BYTES_PER_PIXEL;
        for row in (0..height).step_by(3) {
            let start_col = (row * 37) % (width - span_width_pixels);
            let row_offset = row * row_bytes;
            let start = row_offset + start_col * BGRA_BYTES_PER_PIXEL;
            let end = start + span_width_bytes;
            for byte in &mut current[start..end] {
                *byte ^= 0x5D;
            }
        }

        let compare = row_compare_kernel();
        let expected_baseline = unsafe {
            scan_dirty_pixels_baseline(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                width,
                height,
                compare,
            )
        };
        let expected_single = unsafe {
            scan_dirty_pixels_single_scan(
                current.as_ptr(),
                row_bytes,
                history.as_ptr(),
                row_bytes,
                row_bytes,
                width,
                height,
            )
        };
        assert_eq!(expected_single, expected_baseline);

        let warmup_iters = 20usize;
        let measure_iters = 160usize;
        let mut sink = 0usize;
        for _ in 0..warmup_iters {
            sink ^= unsafe {
                scan_dirty_pixels_baseline(
                    current.as_ptr(),
                    row_bytes,
                    history.as_ptr(),
                    row_bytes,
                    row_bytes,
                    width,
                    height,
                    compare,
                )
            };
            sink ^= unsafe {
                scan_dirty_pixels_single_scan(
                    current.as_ptr(),
                    row_bytes,
                    history.as_ptr(),
                    row_bytes,
                    row_bytes,
                    width,
                    height,
                )
            };
        }
        black_box(sink);

        let baseline_start = Instant::now();
        for _ in 0..measure_iters {
            sink ^= unsafe {
                scan_dirty_pixels_baseline(
                    current.as_ptr(),
                    row_bytes,
                    history.as_ptr(),
                    row_bytes,
                    row_bytes,
                    width,
                    height,
                    compare,
                )
            };
        }
        let baseline_elapsed = baseline_start.elapsed();

        let single_start = Instant::now();
        for _ in 0..measure_iters {
            sink ^= unsafe {
                scan_dirty_pixels_single_scan(
                    current.as_ptr(),
                    row_bytes,
                    history.as_ptr(),
                    row_bytes,
                    row_bytes,
                    width,
                    height,
                )
            };
        }
        let single_elapsed = single_start.elapsed();
        black_box(sink);

        let baseline_ms = baseline_elapsed.as_secs_f64() * 1000.0;
        let single_ms = single_elapsed.as_secs_f64() * 1000.0;
        let improvement_pct = if baseline_ms > 0.0 {
            ((baseline_ms - single_ms) / baseline_ms) * 100.0
        } else {
            0.0
        };

        println!(
            "gdi span single-scan benchmark: baseline={baseline_ms:.3} ms single={single_ms:.3} ms improvement={improvement_pct:.2}%"
        );

        assert!(
            single_ms <= baseline_ms * 1.02,
            "single-scan path regressed: baseline={baseline_ms:.3}ms single={single_ms:.3}ms ({improvement_pct:.2}% improvement)"
        );
    }
}

mod f16;
mod parallel;
mod scalar;
#[cfg(target_arch = "x86_64")]
mod simd_x86;

use crate::frame::CapturePixelFormat;
pub(crate) use f16::{
    HDR_LUMA_LUT_SIZE, HdrPreparedContext, build_bt2390_luma_lut, prepare_hdr_context_cached,
};
use parallel::{install_conversion_pool, parallel_chunk_pixels, should_parallelize};
use std::sync::OnceLock;

/// Initialize allocation-light conversion state such as lookup tables and
/// SIMD dispatch. This deliberately does not create the Rayon worker pool,
/// so it is safe to use while preparing an idle snapshot session.
pub(crate) fn warmup_dispatch() {
    f16::warmup_lut();
    let _ = bgra_kernel();
    let _ = bgra_opaque_kernel();
    let _ = bgra_kernel_nt();
    let _ = bgra_opaque_kernel_nt();
    let _ = bgra_kernel_nt_nofence();
    let _ = bgra_opaque_kernel_nt_nofence();
    let _ = f16_kernel();
    let _ = f16_opaque_kernel();
    let _ = f16_kernel_nt();
    let _ = f16_opaque_kernel_nt();
    let _ = f16_kernel_nt_nofence();
    let _ = f16_opaque_kernel_nt_nofence();
    let _ = f16_hdr_prepared_kernel();
    let _ = f16_hdr_prepared_opaque_kernel();
}

/// Initialize the complete conversion runtime so the first capture doesn't
/// pay the worker-pool creation cost. Safe to call multiple times; only the
/// first call performs initialization.
pub fn warmup() {
    warmup_dispatch();
    parallel::warmup_pool(CONVERSION_PARALLEL_MAX_WORKERS);
}

#[inline]
pub(crate) fn with_conversion_pool<F>(max_workers: usize, job: F)
where
    F: FnOnce() + Send,
{
    install_conversion_pool(max_workers, job);
}

#[inline(always)]
pub(crate) fn should_parallelize_work(
    pixel_count: usize,
    min_pixels: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
) -> bool {
    should_parallelize(pixel_count, min_pixels, min_chunk_pixels, max_workers)
}

const BGRA_PARALLEL_MIN_PIXELS: usize = 524_288;
const BGRA_PARALLEL_MIN_CHUNK_PIXELS: usize = 131_072;
const BGRA_PARALLEL_MAX_WORKERS: usize = usize::MAX;

/// Lower threshold for the non-temporal path used where `src != dst` is
/// guaranteed (GDI capture, DXGI staging->frame).  256K pixels lets us
/// parallelise earlier while still keeping chunks large enough to
/// amortise rayon overhead.
const BGRA_NT_PARALLEL_MIN_PIXELS: usize = 262_144;
const BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS: usize = 65_536;

const F16_PARALLEL_MIN_PIXELS: usize = 262_144;
const F16_PARALLEL_MIN_CHUNK_PIXELS: usize = 65_536;
const F16_PARALLEL_MAX_WORKERS: usize = usize::MAX;
const CONVERSION_PARALLEL_MAX_WORKERS: usize = usize::MAX;

type PixelKernel = unsafe fn(*const u8, *mut u8, usize);
type HdrPreparedPixelKernel = unsafe fn(*const u8, *mut u8, usize, &HdrPreparedContext);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SurfacePixelFormat {
    Bgra8,
    Rgba8,
    Rgba16Float,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum HdrInputModel {
    /// Windows Advanced Color desktop capture source:
    /// scRGB linear (R16G16B16A16_FLOAT), BT.709 primaries.
    #[default]
    WindowsScRgbLinear,
}

/// Immutable per-frame HDR conversion context.
///
/// This context is auto-derived by the Windows backend from display metadata.
/// It intentionally excludes user-tunable curve parameters so the HDR->SDR
/// pipeline follows one consistent standard flow.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct HdrFrameContext {
    /// SDR white level in nits reported by the platform. On Windows this
    /// comes from DISPLAYCONFIG_SDR_WHITE_LEVEL converted to nits.
    pub sdr_white_nits: f32,
    /// Peak luminance capability of the HDR display in nits.
    pub hdr_peak_nits: f32,
    /// When `true`, HDR luma mapping may use LUT approximation.
    pub tonemap_use_lut: bool,
    /// Declares how input pixel values should be interpreted.
    pub input_model: HdrInputModel,
}

impl HdrFrameContext {
    pub(crate) fn sanitized(self) -> Self {
        let sdr_white_nits = if self.sdr_white_nits.is_finite() {
            self.sdr_white_nits.max(1.0)
        } else {
            80.0
        };
        let hdr_peak_nits = if self.hdr_peak_nits.is_finite() {
            self.hdr_peak_nits.max(1.0)
        } else {
            1000.0
        };
        Self {
            sdr_white_nits,
            hdr_peak_nits,
            tonemap_use_lut: self.tonemap_use_lut,
            input_model: self.input_model,
        }
    }
}

impl Default for HdrFrameContext {
    /// Conservative metadata fallbacks for Windows scRGB capture.
    fn default() -> Self {
        Self {
            sdr_white_nits: 80.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: true,
            input_model: HdrInputModel::WindowsScRgbLinear,
        }
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SurfaceConversionOptions {
    pub hdr_to_sdr: Option<HdrFrameContext>,
    pub force_opaque_alpha: bool,
    pub output_pixel_format: CapturePixelFormat,
}

#[inline(always)]
fn requires_channel_swap(format: SurfacePixelFormat, output: CapturePixelFormat) -> bool {
    matches!(
        format,
        SurfacePixelFormat::Bgra8 | SurfacePixelFormat::Rgba8
    ) && ((format == SurfacePixelFormat::Bgra8 && output == CapturePixelFormat::Rgba8)
        || (format == SurfacePixelFormat::Rgba8 && output == CapturePixelFormat::Bgra8))
}

#[inline(always)]
pub(crate) fn conversion_fuses_opaque_alpha(
    format: SurfacePixelFormat,
    options: SurfaceConversionOptions,
) -> bool {
    options.force_opaque_alpha
        && matches!(
            format,
            SurfacePixelFormat::Bgra8 | SurfacePixelFormat::Rgba16Float
        )
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct SurfaceLayout {
    pub(crate) src: *const u8,
    pub(crate) src_pitch: usize,
    pub(crate) dst: *mut u8,
    pub(crate) dst_pitch: usize,
    pub(crate) width: usize,
    pub(crate) height: usize,
}

impl SurfaceLayout {
    pub(crate) const fn new(
        src: *const u8,
        src_pitch: usize,
        dst: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
    ) -> Self {
        Self {
            src,
            src_pitch,
            dst,
            dst_pitch,
            width,
            height,
        }
    }

    fn is_empty(self) -> bool {
        self.width == 0 || self.height == 0
    }

    fn total_pixels(self) -> usize {
        self.width
            .checked_mul(self.height)
            .expect("surface pixel count overflow")
    }

    fn source_row_bytes(self, src_bytes_per_pixel: usize) -> usize {
        self.width
            .checked_mul(src_bytes_per_pixel)
            .expect("surface row byte overflow")
    }

    fn destination_row_bytes(self) -> usize {
        self.width
            .checked_mul(4)
            .expect("surface row byte overflow")
    }

    fn assert_pitches(self, src_bytes_per_pixel: usize) -> (usize, usize) {
        let src_row_bytes = self.source_row_bytes(src_bytes_per_pixel);
        let dst_row_bytes = self.destination_row_bytes();
        assert!(
            self.src_pitch >= src_row_bytes,
            "source pitch too small: pitch={}, required={}",
            self.src_pitch,
            src_row_bytes
        );
        assert!(
            self.dst_pitch >= dst_row_bytes,
            "destination pitch too small: pitch={}, required={}",
            self.dst_pitch,
            dst_row_bytes
        );
        (src_row_bytes, dst_row_bytes)
    }

    fn is_contiguous(self, src_row_bytes: usize, dst_row_bytes: usize) -> bool {
        self.src_pitch == src_row_bytes && self.dst_pitch == dst_row_bytes
    }

    fn allow_parallel_rows(self) -> bool {
        let maybe_src_total = self.src_pitch.checked_mul(self.height);
        let maybe_dst_total = self.dst_pitch.checked_mul(self.height);
        maybe_src_total
            .zip(maybe_dst_total)
            .map(|(src_total, dst_total)| {
                !parallel::ranges_overlap(self.src, src_total, self.dst, dst_total)
            })
            .unwrap_or(false)
    }
}

#[derive(Clone, Copy)]
struct ParallelConfig {
    min_pixels: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
}

#[derive(Clone, Copy)]
struct SurfaceFormatPlan {
    src_bytes_per_pixel: usize,
    contiguous_kernel: PixelKernel,
    row_kernel: PixelKernel,
    /// Non-temporal row kernel for non-overlapping buffers where the
    /// destination won't be read back before the next capture.
    row_kernel_nt: PixelKernel,
    /// Non-temporal row kernel variant that defers `_mm_sfence` to the
    /// caller so multi-row conversions can issue one fence per chunk
    /// instead of one fence per row.
    row_kernel_nt_nofence: PixelKernel,
    parallel: ParallelConfig,
}

#[derive(Clone, Copy)]
pub(crate) struct SurfaceRowConverter {
    format: SurfacePixelFormat,
    output_pixel_format: CapturePixelFormat,
    plan: SurfaceFormatPlan,
    hdr_params: Option<HdrFrameContext>,
    force_opaque_alpha: bool,
}

impl SurfaceRowConverter {
    #[inline]
    pub(crate) fn new(format: SurfacePixelFormat, options: SurfaceConversionOptions) -> Self {
        let hdr_params = if format == SurfacePixelFormat::Rgba16Float {
            options.hdr_to_sdr.map(HdrFrameContext::sanitized)
        } else {
            None
        };
        Self {
            format,
            output_pixel_format: options.output_pixel_format,
            plan: surface_format_plan(format, options),
            hdr_params,
            force_opaque_alpha: options.force_opaque_alpha,
        }
    }

    #[inline(always)]
    pub(crate) unsafe fn convert_rows_unchecked(
        self,
        src: *const u8,
        src_pitch: usize,
        dst: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
    ) {
        unsafe {
            self.convert_rows_impl(src, src_pitch, dst, dst_pitch, width, height, false);
        }
    }

    #[inline(always)]
    pub(crate) unsafe fn convert_rows_maybe_parallel_unchecked(
        self,
        src: *const u8,
        src_pitch: usize,
        dst: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
    ) {
        unsafe {
            self.convert_rows_impl(src, src_pitch, dst, dst_pitch, width, height, true);
        }
    }

    #[inline(always)]
    unsafe fn convert_rows_impl(
        self,
        src: *const u8,
        src_pitch: usize,
        dst: *mut u8,
        dst_pitch: usize,
        width: usize,
        height: usize,
        allow_parallel: bool,
    ) {
        let layout = SurfaceLayout::new(src, src_pitch, dst, dst_pitch, width, height);
        if layout.is_empty() {
            return;
        }

        if self.format == SurfacePixelFormat::Rgba16Float
            && let Some(params) = self.hdr_params
        {
            let _ = layout.assert_pitches(self.plan.src_bytes_per_pixel);
            let total_pixels = layout.total_pixels();

            let prepared = prepare_hdr_context_cached(params);
            let kernel = if self.force_opaque_alpha {
                f16_hdr_prepared_opaque_kernel()
            } else {
                f16_hdr_prepared_kernel()
            };
            if allow_parallel
                && let Some(chunks) =
                    maybe_parallel_row_chunks(layout, self.plan.parallel, total_pixels)
            {
                let prepared_for_parallel = prepared.clone();
                unsafe {
                    run_rows_parallel_with(
                        layout,
                        chunks,
                        self.plan.parallel.max_workers,
                        move |row_src, row_dst, row_width| {
                            kernel(row_src, row_dst, row_width, &prepared_for_parallel);
                            if self.output_pixel_format == CapturePixelFormat::Bgra8 {
                                swap_rgba_to_bgra_in_place(
                                    std::slice::from_raw_parts_mut(row_dst, row_width * 4),
                                    row_width,
                                );
                            }
                        },
                    );
                }
                return;
            }
            unsafe {
                run_rows_serial_with(layout, move |row_src, row_dst, row_width| {
                    kernel(row_src, row_dst, row_width, &prepared);
                    if self.output_pixel_format == CapturePixelFormat::Bgra8 {
                        swap_rgba_to_bgra_in_place(
                            std::slice::from_raw_parts_mut(row_dst, row_width * 4),
                            row_width,
                        );
                    }
                });
            }
            return;
        }

        if self.format == SurfacePixelFormat::Rgba16Float
            && self.output_pixel_format == CapturePixelFormat::Bgra8
        {
            let _ = layout.assert_pitches(self.plan.src_bytes_per_pixel);
            let total_pixels = layout.total_pixels();
            let kernel = self.plan.row_kernel;
            if allow_parallel
                && let Some(chunks) =
                    maybe_parallel_row_chunks(layout, self.plan.parallel, total_pixels)
            {
                unsafe {
                    run_rows_parallel_with(
                        layout,
                        chunks,
                        self.plan.parallel.max_workers,
                        move |row_src, row_dst, row_width| {
                            kernel(row_src, row_dst, row_width);
                            swap_rgba_to_bgra_in_place(
                                std::slice::from_raw_parts_mut(row_dst, row_width * 4),
                                row_width,
                            );
                        },
                    );
                }
                return;
            }
            unsafe {
                run_rows_serial_with(layout, move |row_src, row_dst, row_width| {
                    kernel(row_src, row_dst, row_width);
                    swap_rgba_to_bgra_in_place(
                        std::slice::from_raw_parts_mut(row_dst, row_width * 4),
                        row_width,
                    );
                });
            }
            return;
        }

        let _ = layout.assert_pitches(self.plan.src_bytes_per_pixel);
        let total_pixels = layout.total_pixels();
        let bgra_row_nt_kernel = if requires_channel_swap(self.format, self.output_pixel_format) {
            bgra_nt_kernel_for_rows(
                layout.dst as *const u8,
                layout.dst_pitch,
                self.force_opaque_alpha,
            )
        } else {
            None
        };
        let use_nt = layout.allow_parallel_rows()
            && total_pixels >= NT_STORE_MIN_PIXELS
            && match self.format {
                SurfacePixelFormat::Bgra8 => bgra_row_nt_kernel.is_some(),
                SurfacePixelFormat::Rgba8 => {
                    nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch)
                }
                SurfacePixelFormat::Rgba16Float => {
                    nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch)
                        && f16_nt_supported()
                }
            };

        if allow_parallel
            && let Some(chunks) =
                maybe_parallel_row_chunks(layout, self.plan.parallel, total_pixels)
        {
            let kernel = if use_nt {
                bgra_row_nt_kernel.unwrap_or(self.plan.row_kernel_nt)
            } else {
                self.plan.row_kernel
            };
            unsafe {
                run_rows_parallel(layout, kernel, chunks, self.plan.parallel.max_workers);
            }
            return;
        }

        let bgra_row_nt_kernel_nofence =
            if requires_channel_swap(self.format, self.output_pixel_format) {
                bgra_nt_kernel_for_rows_nofence(
                    layout.dst as *const u8,
                    layout.dst_pitch,
                    self.force_opaque_alpha,
                )
            } else {
                None
            };
        let kernel = if use_nt {
            bgra_row_nt_kernel_nofence.unwrap_or(self.plan.row_kernel_nt_nofence)
        } else {
            self.plan.row_kernel
        };
        unsafe {
            if use_nt {
                run_rows_serial_nt(layout, kernel);
            } else {
                run_rows_serial(layout, kernel);
            }
        }
    }
}

#[derive(Clone, Copy)]
struct RowChunkPlan {
    chunk_rows: usize,
    chunk_count: usize,
}

pub fn convert_row_to_rgba(
    format: SurfacePixelFormat,
    src_row: &[u8],
    dst_row: &mut [u8],
    pixel_count: usize,
) {
    convert_row_to_rgba_with_options(
        format,
        src_row,
        dst_row,
        pixel_count,
        SurfaceConversionOptions::default(),
    );
}

pub fn convert_row_to_rgba_with_options(
    format: SurfacePixelFormat,
    src_row: &[u8],
    dst_row: &mut [u8],
    pixel_count: usize,
    options: SurfaceConversionOptions,
) {
    match format {
        SurfacePixelFormat::Bgra8 => {
            if options.output_pixel_format == CapturePixelFormat::Bgra8 {
                copy_bgra_with_options(src_row, dst_row, pixel_count, options.force_opaque_alpha)
            } else if options.force_opaque_alpha {
                convert_bgra_to_rgba_opaque(src_row, dst_row, pixel_count)
            } else {
                convert_bgra_to_rgba(src_row, dst_row, pixel_count)
            }
        }
        SurfacePixelFormat::Rgba8 => {
            if options.output_pixel_format == CapturePixelFormat::Bgra8 {
                convert_bgra_to_rgba(src_row, dst_row, pixel_count);
            } else {
                let byte_count = pixel_count * 4;
                dst_row[..byte_count].copy_from_slice(&src_row[..byte_count]);
            }
        }
        SurfacePixelFormat::Rgba16Float => {
            if let Some(params) = options.hdr_to_sdr {
                let required_src = pixel_count
                    .checked_mul(8)
                    .expect("pixel_count overflow when converting HDR RGBA16F to sRGB");
                let required_dst = pixel_count
                    .checked_mul(4)
                    .expect("pixel_count overflow when converting HDR RGBA16F to sRGB");
                assert!(
                    src_row.len() >= required_src,
                    "RGBA16F source buffer too small: got {}, need at least {} bytes",
                    src_row.len(),
                    required_src
                );
                assert!(
                    dst_row.len() >= required_dst,
                    "RGBA destination buffer too small: got {}, need at least {} bytes",
                    dst_row.len(),
                    required_dst
                );
                let prepared = prepare_hdr_context_cached(params.sanitized());
                let kernel = if options.force_opaque_alpha {
                    f16_hdr_prepared_opaque_kernel()
                } else {
                    f16_hdr_prepared_kernel()
                };
                unsafe {
                    kernel(
                        src_row.as_ptr(),
                        dst_row.as_mut_ptr(),
                        pixel_count,
                        &prepared,
                    );
                }
                if options.output_pixel_format == CapturePixelFormat::Bgra8 {
                    swap_rgba_to_bgra_in_place(dst_row, pixel_count);
                }
            } else if options.force_opaque_alpha {
                convert_f16_rgba_to_srgb_opaque(src_row, dst_row, pixel_count);
                if options.output_pixel_format == CapturePixelFormat::Bgra8 {
                    swap_rgba_to_bgra_in_place(dst_row, pixel_count);
                }
            } else {
                convert_f16_rgba_to_srgb(src_row, dst_row, pixel_count);
                if options.output_pixel_format == CapturePixelFormat::Bgra8 {
                    swap_rgba_to_bgra_in_place(dst_row, pixel_count);
                }
            }
        }
    }
}

/// Convert a 2D surface with arbitrary source/destination pitch into RGBA8.
///
/// This mirrors the conversion path used by DXGI staging readback, where
/// `src_pitch` is often padded beyond `width * bytes_per_pixel`.
pub fn convert_surface_to_rgba(
    format: SurfacePixelFormat,
    src: &[u8],
    src_pitch: usize,
    dst: &mut [u8],
    dst_pitch: usize,
    width: usize,
    height: usize,
    options: SurfaceConversionOptions,
) {
    if width == 0 || height == 0 {
        return;
    }

    let src_bpp = match format {
        SurfacePixelFormat::Bgra8 | SurfacePixelFormat::Rgba8 => 4usize,
        SurfacePixelFormat::Rgba16Float => 8usize,
    };
    let src_row_bytes = width
        .checked_mul(src_bpp)
        .expect("width overflow while validating source surface");
    let dst_row_bytes = width
        .checked_mul(4)
        .expect("width overflow while validating destination surface");
    assert!(
        src_pitch >= src_row_bytes,
        "source pitch too small: pitch={}, required={}",
        src_pitch,
        src_row_bytes
    );
    assert!(
        dst_pitch >= dst_row_bytes,
        "destination pitch too small: pitch={}, required={}",
        dst_pitch,
        dst_row_bytes
    );

    let src_required = src_pitch
        .checked_mul(height.saturating_sub(1))
        .and_then(|base| base.checked_add(src_row_bytes))
        .expect("source surface size overflow");
    let dst_required = dst_pitch
        .checked_mul(height.saturating_sub(1))
        .and_then(|base| base.checked_add(dst_row_bytes))
        .expect("destination surface size overflow");
    assert!(
        src.len() >= src_required,
        "source surface buffer too small: got {}, need at least {} bytes",
        src.len(),
        src_required
    );
    assert!(
        dst.len() >= dst_required,
        "destination surface buffer too small: got {}, need at least {} bytes",
        dst.len(),
        dst_required
    );

    unsafe {
        convert_surface_to_rgba_unchecked(
            format,
            SurfaceLayout::new(
                src.as_ptr(),
                src_pitch,
                dst.as_mut_ptr(),
                dst_pitch,
                width,
                height,
            ),
            options,
        );
    }
}

fn surface_format_plan(
    format: SurfacePixelFormat,
    options: SurfaceConversionOptions,
) -> SurfaceFormatPlan {
    match format {
        SurfacePixelFormat::Bgra8 if options.output_pixel_format == CapturePixelFormat::Bgra8 => {
            SurfaceFormatPlan {
                src_bytes_per_pixel: 4,
                contiguous_kernel: if options.force_opaque_alpha {
                    copy_bgra_opaque_unchecked
                } else {
                    memcpy_rgba_unchecked
                },
                row_kernel: if options.force_opaque_alpha {
                    copy_bgra_opaque_unchecked
                } else {
                    memcpy_rgba_unchecked
                },
                row_kernel_nt: memcpy_rgba_nt_unchecked,
                row_kernel_nt_nofence: memcpy_rgba_nt_nofence_unchecked,
                parallel: ParallelConfig {
                    min_pixels: usize::MAX,
                    min_chunk_pixels: usize::MAX,
                    max_workers: 1,
                },
            }
        }
        SurfacePixelFormat::Bgra8 => SurfaceFormatPlan {
            src_bytes_per_pixel: 4,
            contiguous_kernel: if options.force_opaque_alpha {
                convert_bgra_to_rgba_opaque_unchecked
            } else {
                convert_bgra_to_rgba_unchecked
            },
            row_kernel: if options.force_opaque_alpha {
                bgra_opaque_kernel()
            } else {
                bgra_kernel()
            },
            row_kernel_nt: if options.force_opaque_alpha {
                bgra_opaque_kernel_nt()
            } else {
                bgra_kernel_nt()
            },
            row_kernel_nt_nofence: if options.force_opaque_alpha {
                bgra_opaque_kernel_nt_nofence()
            } else {
                bgra_kernel_nt_nofence()
            },
            parallel: ParallelConfig {
                min_pixels: BGRA_PARALLEL_MIN_PIXELS,
                min_chunk_pixels: BGRA_PARALLEL_MIN_CHUNK_PIXELS,
                max_workers: BGRA_PARALLEL_MAX_WORKERS,
            },
        },
        SurfacePixelFormat::Rgba8 if options.output_pixel_format == CapturePixelFormat::Bgra8 => {
            SurfaceFormatPlan {
                src_bytes_per_pixel: 4,
                contiguous_kernel: if options.force_opaque_alpha {
                    convert_bgra_to_rgba_opaque_unchecked
                } else {
                    convert_bgra_to_rgba_unchecked
                },
                row_kernel: if options.force_opaque_alpha {
                    bgra_opaque_kernel()
                } else {
                    bgra_kernel()
                },
                row_kernel_nt: if options.force_opaque_alpha {
                    bgra_opaque_kernel_nt()
                } else {
                    bgra_kernel_nt()
                },
                row_kernel_nt_nofence: if options.force_opaque_alpha {
                    bgra_opaque_kernel_nt_nofence()
                } else {
                    bgra_kernel_nt_nofence()
                },
                parallel: ParallelConfig {
                    min_pixels: BGRA_PARALLEL_MIN_PIXELS,
                    min_chunk_pixels: BGRA_PARALLEL_MIN_CHUNK_PIXELS,
                    max_workers: BGRA_PARALLEL_MAX_WORKERS,
                },
            }
        }
        SurfacePixelFormat::Rgba8 => SurfaceFormatPlan {
            src_bytes_per_pixel: 4,
            contiguous_kernel: memcpy_rgba_unchecked,
            row_kernel: memcpy_rgba_unchecked,
            row_kernel_nt: memcpy_rgba_nt_unchecked,
            row_kernel_nt_nofence: memcpy_rgba_nt_nofence_unchecked,
            parallel: ParallelConfig {
                min_pixels: usize::MAX, // never parallelise a plain memcpy
                min_chunk_pixels: usize::MAX,
                max_workers: 1,
            },
        },
        SurfacePixelFormat::Rgba16Float => SurfaceFormatPlan {
            src_bytes_per_pixel: 8,
            contiguous_kernel: if options.force_opaque_alpha {
                convert_f16_rgba_to_srgb_opaque_unchecked
            } else {
                convert_f16_rgba_to_srgb_unchecked
            },
            row_kernel: if options.force_opaque_alpha {
                f16_opaque_kernel()
            } else {
                f16_kernel()
            },
            row_kernel_nt: if options.force_opaque_alpha {
                f16_opaque_kernel_nt()
            } else {
                f16_kernel_nt()
            },
            row_kernel_nt_nofence: if options.force_opaque_alpha {
                f16_opaque_kernel_nt_nofence()
            } else {
                f16_kernel_nt_nofence()
            },
            parallel: ParallelConfig {
                min_pixels: F16_PARALLEL_MIN_PIXELS,
                min_chunk_pixels: F16_PARALLEL_MIN_CHUNK_PIXELS,
                max_workers: F16_PARALLEL_MAX_WORKERS,
            },
        },
    }
}

fn maybe_parallel_row_chunks(
    layout: SurfaceLayout,
    parallel: ParallelConfig,
    total_pixels: usize,
) -> Option<RowChunkPlan> {
    if !layout.allow_parallel_rows()
        || !should_parallelize(
            total_pixels,
            parallel.min_pixels,
            parallel.min_chunk_pixels,
            parallel.max_workers,
        )
    {
        return None;
    }
    row_chunk_plan(
        layout,
        parallel.min_chunk_pixels,
        parallel.max_workers,
        total_pixels,
    )
}

fn maybe_parallel_pixel_chunk_size(parallel: ParallelConfig, total_pixels: usize) -> Option<usize> {
    if !should_parallelize(
        total_pixels,
        parallel.min_pixels,
        parallel.min_chunk_pixels,
        parallel.max_workers,
    ) {
        return None;
    }
    parallel_chunk_pixels(
        total_pixels,
        parallel.min_chunk_pixels,
        parallel.max_workers,
    )
}

fn row_chunk_plan(
    layout: SurfaceLayout,
    min_chunk_pixels: usize,
    max_workers: usize,
    total_pixels: usize,
) -> Option<RowChunkPlan> {
    let chunk_pixels = parallel_chunk_pixels(total_pixels, min_chunk_pixels, max_workers)?;
    let chunk_rows = (chunk_pixels / layout.width).max(1);
    Some(RowChunkPlan {
        chunk_rows,
        chunk_count: layout.height.div_ceil(chunk_rows),
    })
}

unsafe fn run_rows_serial_with<F>(layout: SurfaceLayout, mut row_fn: F)
where
    F: FnMut(*const u8, *mut u8, usize),
{
    for row in 0..layout.height {
        let src = unsafe { layout.src.add(row * layout.src_pitch) };
        let dst = unsafe { layout.dst.add(row * layout.dst_pitch) };
        row_fn(src, dst, layout.width);
    }
}

unsafe fn run_rows_parallel_with<F>(
    layout: SurfaceLayout,
    chunks: RowChunkPlan,
    max_workers: usize,
    row_fn: F,
) where
    F: Fn(*const u8, *mut u8, usize) + Send + Sync,
{
    let src_addr = layout.src as usize;
    let dst_addr = layout.dst as usize;

    use rayon::prelude::*;
    install_conversion_pool(max_workers, || {
        (0..chunks.chunk_count)
            .into_par_iter()
            .for_each(|chunk_idx| {
                let start_row = chunk_idx * chunks.chunk_rows;
                let rows = (layout.height - start_row).min(chunks.chunk_rows);
                for row_offset in 0..rows {
                    let row = start_row + row_offset;
                    row_fn(
                        (src_addr + row * layout.src_pitch) as *const u8,
                        (dst_addr + row * layout.dst_pitch) as *mut u8,
                        layout.width,
                    );
                }
            });
    });
}

unsafe fn run_pixels_parallel_with<F>(
    src: *const u8,
    src_stride_bytes: usize,
    dst: *mut u8,
    dst_stride_bytes: usize,
    total_pixels: usize,
    chunk_pixels: usize,
    max_workers: usize,
    pixel_fn: F,
) where
    F: Fn(*const u8, *mut u8, usize) + Send + Sync,
{
    let chunk_count = total_pixels.div_ceil(chunk_pixels);
    let src_addr = src as usize;
    let dst_addr = dst as usize;

    use rayon::prelude::*;
    install_conversion_pool(max_workers, || {
        (0..chunk_count).into_par_iter().for_each(|chunk_idx| {
            let start = chunk_idx * chunk_pixels;
            let len = (total_pixels - start).min(chunk_pixels);
            pixel_fn(
                (src_addr + start * src_stride_bytes) as *const u8,
                (dst_addr + start * dst_stride_bytes) as *mut u8,
                len,
            );
        });
    });
}

unsafe fn run_rows_serial(layout: SurfaceLayout, kernel: PixelKernel) {
    unsafe {
        run_rows_serial_with(layout, |src, dst, width| kernel(src, dst, width));
    }
}

unsafe fn run_rows_parallel(
    layout: SurfaceLayout,
    kernel: PixelKernel,
    chunks: RowChunkPlan,
    max_workers: usize,
) {
    unsafe {
        run_rows_parallel_with(layout, chunks, max_workers, move |src, dst, width| {
            kernel(src, dst, width);
        });
    }
}

#[inline(always)]
fn nt_store_sfence() {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        std::arch::x86_64::_mm_sfence();
    }
}

unsafe fn run_rows_serial_nt(layout: SurfaceLayout, kernel: PixelKernel) {
    unsafe {
        run_rows_serial(layout, kernel);
    }
    nt_store_sfence();
}

/// Minimum pixel count for non-temporal stores to be beneficial.
/// Below this threshold the destination buffer likely fits in L3 cache
/// and temporal stores are faster (avoids write-combine overhead).
/// ~128K pixels ~= 512 KB at 4 bytes/pixel - well below typical L3
/// sizes but large enough that write-allocate traffic starts to hurt.
/// Decoupled from the parallelisation thresholds so that medium-
/// resolution captures (e.g. 720p-1080p) still benefit from NT stores
/// in the staging->frame path where src != dst is guaranteed.
const NT_STORE_MIN_PIXELS: usize = 131_072;

#[inline(always)]
fn nt_store_alignment_bytes() -> usize {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return 64;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return 32;
        }
        if std::arch::is_x86_feature_detected!("sse2") {
            return 16;
        }
        1
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        1
    }
}

#[inline(always)]
fn ptr_is_aligned(ptr: *const u8, alignment: usize) -> bool {
    alignment <= 1 || ((ptr as usize) & (alignment - 1)) == 0
}

#[inline(always)]
fn nt_destination_is_aligned(dst: *const u8) -> bool {
    ptr_is_aligned(dst, nt_store_alignment_bytes())
}

#[inline(always)]
fn nt_rows_are_aligned(dst: *const u8, dst_pitch: usize) -> bool {
    let alignment = nt_store_alignment_bytes();
    alignment <= 1 || (ptr_is_aligned(dst, alignment) && dst_pitch.is_multiple_of(alignment))
}

#[inline(always)]
fn f16_nt_supported() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        (std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c"))
            || (std::arch::is_x86_feature_detected!("avx2")
                && std::arch::is_x86_feature_detected!("f16c"))
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        false
    }
}

#[cfg(target_arch = "x86_64")]
#[derive(Clone, Copy)]
struct BgraNtKernelSet {
    avx512: Option<PixelKernel>,
    avx2: Option<PixelKernel>,
    ssse3: Option<PixelKernel>,
    avx512_nofence: Option<PixelKernel>,
    avx2_nofence: Option<PixelKernel>,
    ssse3_nofence: Option<PixelKernel>,
}

#[cfg(target_arch = "x86_64")]
#[inline(always)]
fn bgra_nt_kernel_set(force_opaque_alpha: bool) -> &'static BgraNtKernelSet {
    static REGULAR: OnceLock<BgraNtKernelSet> = OnceLock::new();
    static OPAQUE: OnceLock<BgraNtKernelSet> = OnceLock::new();
    let kernel_slot = if force_opaque_alpha {
        &OPAQUE
    } else {
        &REGULAR
    };
    kernel_slot.get_or_init(|| BgraNtKernelSet {
        avx512: if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_avx512_nt_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_avx512_nt_unchecked
            })
        } else {
            None
        },
        avx2: if std::arch::is_x86_feature_detected!("avx2") {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_avx2_nt_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_avx2_nt_unchecked
            })
        } else {
            None
        },
        ssse3: if std::arch::is_x86_feature_detected!("ssse3") {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_ssse3_nt_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_ssse3_nt_unchecked
            })
        } else {
            None
        },
        avx512_nofence: if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_avx512_nt_nofence_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_avx512_nt_nofence_unchecked
            })
        } else {
            None
        },
        avx2_nofence: if std::arch::is_x86_feature_detected!("avx2") {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_avx2_nt_nofence_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_avx2_nt_nofence_unchecked
            })
        } else {
            None
        },
        ssse3_nofence: if std::arch::is_x86_feature_detected!("ssse3") {
            Some(if force_opaque_alpha {
                simd_x86::convert_bgra_to_rgba_ssse3_nt_nofence_opaque_unchecked
            } else {
                simd_x86::convert_bgra_to_rgba_ssse3_nt_nofence_unchecked
            })
        } else {
            None
        },
    })
}

#[inline(always)]
fn bgra_nt_kernel_for_destination(dst: *const u8, force_opaque_alpha: bool) -> Option<PixelKernel> {
    #[cfg(target_arch = "x86_64")]
    {
        let _ = dst;
        let kernels = bgra_nt_kernel_set(force_opaque_alpha);
        if let Some(kernel) = kernels.avx512 {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.avx2 {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.ssse3 {
            return Some(kernel);
        }
        None
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        let _ = (dst, force_opaque_alpha);
        None
    }
}

#[inline(always)]
fn bgra_nt_kernel_for_rows(
    dst: *const u8,
    dst_pitch: usize,
    force_opaque_alpha: bool,
) -> Option<PixelKernel> {
    #[cfg(target_arch = "x86_64")]
    {
        let _ = (dst, dst_pitch);
        let kernels = bgra_nt_kernel_set(force_opaque_alpha);
        if let Some(kernel) = kernels.avx512 {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.avx2 {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.ssse3 {
            return Some(kernel);
        }
        None
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        let _ = (dst, dst_pitch, force_opaque_alpha);
        None
    }
}

#[inline(always)]
fn bgra_nt_kernel_for_rows_nofence(
    dst: *const u8,
    dst_pitch: usize,
    force_opaque_alpha: bool,
) -> Option<PixelKernel> {
    #[cfg(target_arch = "x86_64")]
    {
        let _ = (dst, dst_pitch);
        let kernels = bgra_nt_kernel_set(force_opaque_alpha);
        if let Some(kernel) = kernels.avx512_nofence {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.avx2_nofence {
            return Some(kernel);
        }
        if let Some(kernel) = kernels.ssse3_nofence {
            return Some(kernel);
        }
        None
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        let _ = (dst, dst_pitch, force_opaque_alpha);
        None
    }
}

#[derive(Clone, Copy)]
pub(crate) struct BgraDirtyRectKernel {
    kernel: PixelKernel,
    needs_post_fence: bool,
}

impl BgraDirtyRectKernel {
    #[inline(always)]
    fn run(self, src: *const u8, dst: *mut u8, pixel_count: usize) {
        unsafe {
            (self.kernel)(src, dst, pixel_count);
        }
    }

    #[inline(always)]
    fn needs_post_fence(self) -> bool {
        self.needs_post_fence
    }
}

#[inline]
pub(crate) fn select_bgra_dirty_rect_kernel(
    dst: *const u8,
    dst_pitch: usize,
    total_pixels: usize,
    defer_nt_fence: bool,
    force_opaque_alpha: bool,
) -> BgraDirtyRectKernel {
    if total_pixels >= NT_STORE_MIN_PIXELS {
        let nt_kernel = bgra_nt_kernel_for_rows(dst, dst_pitch, force_opaque_alpha);
        if let Some(kernel) = nt_kernel {
            if defer_nt_fence
                && let Some(nofence_kernel) =
                    bgra_nt_kernel_for_rows_nofence(dst, dst_pitch, force_opaque_alpha)
            {
                return BgraDirtyRectKernel {
                    kernel: nofence_kernel,
                    needs_post_fence: true,
                };
            }
            return BgraDirtyRectKernel {
                kernel,
                needs_post_fence: false,
            };
        }
    }

    BgraDirtyRectKernel {
        kernel: if force_opaque_alpha {
            bgra_opaque_kernel()
        } else {
            bgra_kernel()
        },
        needs_post_fence: false,
    }
}

#[inline(always)]
pub(crate) unsafe fn convert_bgra_rows_with_kernel_unchecked(
    kernel: BgraDirtyRectKernel,
    src: *const u8,
    src_pitch: usize,
    dst: *mut u8,
    dst_pitch: usize,
    width: usize,
    height: usize,
) {
    if width == 0 || height == 0 {
        return;
    }

    for row in 0..height {
        let row_src = unsafe { src.add(row * src_pitch) };
        let row_dst = unsafe { dst.add(row * dst_pitch) };
        kernel.run(row_src, row_dst, width);
    }
}

#[inline(always)]
pub(crate) fn finalize_bgra_dirty_rect_kernel(kernel: BgraDirtyRectKernel) {
    if kernel.needs_post_fence() {
        nt_store_sfence();
    }
}

pub(crate) unsafe fn convert_surface_to_rgba_unchecked(
    format: SurfacePixelFormat,
    layout: SurfaceLayout,
    options: SurfaceConversionOptions,
) {
    if layout.is_empty() {
        return;
    }

    if format == SurfacePixelFormat::Rgba16Float
        && let Some(params) = options.hdr_to_sdr
    {
        unsafe {
            convert_f16_surface_to_srgb_hdr_unchecked(
                layout,
                params.sanitized(),
                options.force_opaque_alpha,
                options.output_pixel_format,
            );
        }
        return;
    }

    if format == SurfacePixelFormat::Rgba16Float
        && options.output_pixel_format == CapturePixelFormat::Bgra8
    {
        unsafe {
            SurfaceRowConverter::new(format, options).convert_rows_maybe_parallel_unchecked(
                layout.src,
                layout.src_pitch,
                layout.dst,
                layout.dst_pitch,
                layout.width,
                layout.height,
            );
        }
        return;
    }

    let plan = surface_format_plan(format, options);
    let (src_row_bytes, dst_row_bytes) = layout.assert_pitches(plan.src_bytes_per_pixel);
    let total_pixels = layout.total_pixels();
    let channel_swap = requires_channel_swap(format, options.output_pixel_format);

    if layout.is_contiguous(src_row_bytes, dst_row_bytes) {
        let non_overlapping = !parallel::ranges_overlap(
            layout.src,
            total_pixels * plan.src_bytes_per_pixel,
            layout.dst,
            total_pixels * 4,
        );
        // Only use NT stores when the buffer is large enough that it
        // would thrash the L3 cache with temporal writes.  For smaller
        // surfaces the regular (temporal) path is faster.
        let use_nt = non_overlapping
            && total_pixels >= NT_STORE_MIN_PIXELS
            && match format {
                SurfacePixelFormat::Bgra8 if channel_swap => bgra_nt_kernel_for_destination(
                    layout.dst as *const u8,
                    options.force_opaque_alpha,
                )
                .is_some(),
                SurfacePixelFormat::Bgra8 => false,
                SurfacePixelFormat::Rgba8 => {
                    channel_swap && nt_destination_is_aligned(layout.dst as *const u8)
                }
                SurfacePixelFormat::Rgba16Float => {
                    nt_destination_is_aligned(layout.dst as *const u8) && f16_nt_supported()
                }
            };
        if use_nt {
            if format == SurfacePixelFormat::Bgra8 && channel_swap {
                unsafe {
                    if options.force_opaque_alpha {
                        convert_bgra_to_rgba_opaque_nt_unchecked(
                            layout.src,
                            layout.dst,
                            total_pixels,
                        );
                    } else {
                        convert_bgra_to_rgba_nt_unchecked(layout.src, layout.dst, total_pixels);
                    }
                }
                return;
            }
            if format == SurfacePixelFormat::Rgba16Float {
                unsafe {
                    if options.force_opaque_alpha {
                        convert_f16_rgba_to_srgb_opaque_nt_unchecked(
                            layout.src,
                            layout.dst,
                            total_pixels,
                        );
                    } else {
                        convert_f16_rgba_to_srgb_nt_unchecked(layout.src, layout.dst, total_pixels);
                    }
                }
                return;
            }
        }
        unsafe {
            (plan.contiguous_kernel)(layout.src, layout.dst, total_pixels);
        }
        return;
    }

    if let Some(chunks) = maybe_parallel_row_chunks(layout, plan.parallel, total_pixels) {
        let bgra_row_nt_kernel = if channel_swap {
            bgra_nt_kernel_for_rows(
                layout.dst as *const u8,
                layout.dst_pitch,
                options.force_opaque_alpha,
            )
        } else {
            None
        };
        let use_nt = layout.allow_parallel_rows()
            && total_pixels >= NT_STORE_MIN_PIXELS
            && match format {
                SurfacePixelFormat::Bgra8 => channel_swap && bgra_row_nt_kernel.is_some(),
                SurfacePixelFormat::Rgba8 => {
                    channel_swap && nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch)
                }
                SurfacePixelFormat::Rgba16Float => {
                    nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch)
                        && f16_nt_supported()
                }
            };
        let kernel = if use_nt {
            bgra_row_nt_kernel.unwrap_or(plan.row_kernel_nt)
        } else {
            plan.row_kernel
        };
        unsafe {
            run_rows_parallel(layout, kernel, chunks, plan.parallel.max_workers);
        }
        return;
    }

    let bgra_row_nt_kernel = if channel_swap {
        bgra_nt_kernel_for_rows(
            layout.dst as *const u8,
            layout.dst_pitch,
            options.force_opaque_alpha,
        )
    } else {
        None
    };
    let bgra_row_nt_kernel_nofence = if channel_swap {
        bgra_nt_kernel_for_rows_nofence(
            layout.dst as *const u8,
            layout.dst_pitch,
            options.force_opaque_alpha,
        )
    } else {
        None
    };
    let use_nt = layout.allow_parallel_rows()
        && total_pixels >= NT_STORE_MIN_PIXELS
        && match format {
            SurfacePixelFormat::Bgra8 => channel_swap && bgra_row_nt_kernel.is_some(),
            SurfacePixelFormat::Rgba8 => {
                channel_swap && nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch)
            }
            SurfacePixelFormat::Rgba16Float => {
                nt_rows_are_aligned(layout.dst as *const u8, layout.dst_pitch) && f16_nt_supported()
            }
        };
    let kernel = if use_nt {
        bgra_row_nt_kernel_nofence.unwrap_or(plan.row_kernel_nt_nofence)
    } else {
        plan.row_kernel
    };
    unsafe {
        if use_nt {
            run_rows_serial_nt(layout, kernel);
        } else {
            run_rows_serial(layout, kernel);
        }
    }
}

unsafe fn convert_f16_surface_to_srgb_hdr_unchecked(
    layout: SurfaceLayout,
    params: HdrFrameContext,
    force_opaque_alpha: bool,
    output_pixel_format: CapturePixelFormat,
) {
    let params = params.sanitized();
    let (src_row_bytes, dst_row_bytes) = layout.assert_pitches(8);
    let total_pixels = layout.total_pixels();
    let prepared = prepare_hdr_context_cached(params);
    let kernel = if force_opaque_alpha {
        f16_hdr_prepared_opaque_kernel()
    } else {
        f16_hdr_prepared_kernel()
    };

    if layout.is_contiguous(src_row_bytes, dst_row_bytes) {
        let parallel = ParallelConfig {
            min_pixels: F16_PARALLEL_MIN_PIXELS,
            min_chunk_pixels: F16_PARALLEL_MIN_CHUNK_PIXELS,
            max_workers: F16_PARALLEL_MAX_WORKERS,
        };
        let src_total = total_pixels
            .checked_mul(8)
            .expect("pixel_count overflow while converting HDR RGBA16F source");
        let dst_total = total_pixels
            .checked_mul(4)
            .expect("pixel_count overflow while converting HDR RGBA16F destination");
        if !parallel::ranges_overlap(layout.src, src_total, layout.dst, dst_total)
            && let Some(chunk_pixels) = maybe_parallel_pixel_chunk_size(parallel, total_pixels)
        {
            let prepared_for_parallel = prepared.clone();
            unsafe {
                run_pixels_parallel_with(
                    layout.src,
                    8,
                    layout.dst,
                    4,
                    total_pixels,
                    chunk_pixels,
                    parallel.max_workers,
                    move |src, dst, pixels| {
                        kernel(src, dst, pixels, &prepared_for_parallel);
                        if output_pixel_format == CapturePixelFormat::Bgra8 {
                            swap_rgba_to_bgra_in_place(
                                std::slice::from_raw_parts_mut(dst, pixels * 4),
                                pixels,
                            );
                        }
                    },
                );
            }
        } else {
            unsafe {
                kernel(layout.src, layout.dst, total_pixels, &prepared);
                if output_pixel_format == CapturePixelFormat::Bgra8 {
                    swap_rgba_to_bgra_in_place(
                        std::slice::from_raw_parts_mut(layout.dst, total_pixels * 4),
                        total_pixels,
                    );
                }
            }
        }
        return;
    }

    let parallel = ParallelConfig {
        min_pixels: F16_PARALLEL_MIN_PIXELS,
        min_chunk_pixels: F16_PARALLEL_MIN_CHUNK_PIXELS,
        max_workers: F16_PARALLEL_MAX_WORKERS,
    };
    if let Some(chunks) = maybe_parallel_row_chunks(layout, parallel, total_pixels) {
        let prepared_for_parallel = prepared.clone();
        unsafe {
            run_rows_parallel_with(
                layout,
                chunks,
                parallel.max_workers,
                move |src, dst, width| {
                    kernel(src, dst, width, &prepared_for_parallel);
                    if output_pixel_format == CapturePixelFormat::Bgra8 {
                        swap_rgba_to_bgra_in_place(
                            std::slice::from_raw_parts_mut(dst, width * 4),
                            width,
                        );
                    }
                },
            );
        }
        return;
    }

    unsafe {
        run_rows_serial_with(layout, move |src, dst, width| {
            kernel(src, dst, width, &prepared);
            if output_pixel_format == CapturePixelFormat::Bgra8 {
                swap_rgba_to_bgra_in_place(std::slice::from_raw_parts_mut(dst, width * 4), width);
            }
        });
    }
}

/// Passthrough copy for data that is already in RGBA8 layout.
/// Matches the `PixelKernel` signature so it can be used in
/// `SurfaceFormatPlan`.
unsafe fn memcpy_rgba_unchecked(src: *const u8, dst: *mut u8, pixel_count: usize) {
    unsafe {
        std::ptr::copy_nonoverlapping(src, dst, pixel_count * 4);
    }
}

unsafe fn copy_bgra_opaque_unchecked(src: *const u8, dst: *mut u8, pixel_count: usize) {
    unsafe {
        std::ptr::copy_nonoverlapping(src, dst, pixel_count * 4);
        for index in 0..pixel_count {
            *dst.add(index * 4 + 3) = 255;
        }
    }
}

fn copy_bgra_with_options(src: &[u8], dst: &mut [u8], pixel_count: usize, force_opaque: bool) {
    let byte_count = pixel_count * 4;
    dst[..byte_count].copy_from_slice(&src[..byte_count]);
    if force_opaque {
        for alpha in dst[..byte_count]
            .chunks_exact_mut(4)
            .map(|pixel| &mut pixel[3])
        {
            *alpha = 255;
        }
    }
}

fn swap_rgba_to_bgra_in_place(bytes: &mut [u8], pixel_count: usize) {
    for pixel in bytes[..pixel_count * 4].chunks_exact_mut(4) {
        pixel.swap(0, 2);
    }
}

/// Non-temporal passthrough copy for RGBA8 data.  Uses streaming stores
/// to avoid polluting the cache when the destination won't be read back
/// before the next capture (staging->frame path).
unsafe fn memcpy_rgba_nt_unchecked(src: *const u8, dst: *mut u8, pixel_count: usize) {
    unsafe {
        memcpy_rgba_nt_impl(src, dst, pixel_count, true);
    }
}

unsafe fn memcpy_rgba_nt_nofence_unchecked(src: *const u8, dst: *mut u8, pixel_count: usize) {
    unsafe {
        memcpy_rgba_nt_impl(src, dst, pixel_count, false);
    }
}

unsafe fn memcpy_rgba_nt_impl(src: *const u8, dst: *mut u8, pixel_count: usize, fence: bool) {
    if !nt_destination_is_aligned(dst as *const u8) {
        unsafe {
            std::ptr::copy_nonoverlapping(src, dst, pixel_count * 4);
        }
        return;
    }
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx2") {
            unsafe {
                memcpy_rgba_nt_avx2(src, dst, pixel_count, fence);
            }
            return;
        }
        if std::arch::is_x86_feature_detected!("sse2") {
            unsafe {
                memcpy_rgba_nt_sse2(src, dst, pixel_count, fence);
            }
            return;
        }
    }
    unsafe {
        std::ptr::copy_nonoverlapping(src, dst, pixel_count * 4);
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn memcpy_rgba_nt_avx2(src: *const u8, dst: *mut u8, pixel_count: usize, fence: bool) {
    use std::arch::x86_64::{__m256i, _mm_sfence, _mm256_loadu_si256, _mm256_stream_si256};
    let total_bytes = pixel_count * 4;
    let mut offset = 0usize;
    while offset + 32 <= total_bytes {
        unsafe {
            let v = _mm256_loadu_si256(src.add(offset) as *const __m256i);
            _mm256_stream_si256(dst.add(offset) as *mut __m256i, v);
        }
        offset += 32;
    }
    if fence {
        _mm_sfence();
    }
    if offset < total_bytes {
        unsafe {
            std::ptr::copy_nonoverlapping(src.add(offset), dst.add(offset), total_bytes - offset);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sse2")]
unsafe fn memcpy_rgba_nt_sse2(src: *const u8, dst: *mut u8, pixel_count: usize, fence: bool) {
    use std::arch::x86_64::{__m128i, _mm_loadu_si128, _mm_sfence, _mm_stream_si128};
    let total_bytes = pixel_count * 4;
    let mut offset = 0usize;
    while offset + 16 <= total_bytes {
        unsafe {
            let v = _mm_loadu_si128(src.add(offset) as *const __m128i);
            _mm_stream_si128(dst.add(offset) as *mut __m128i, v);
        }
        offset += 16;
    }
    if fence {
        _mm_sfence();
    }
    if offset < total_bytes {
        unsafe {
            std::ptr::copy_nonoverlapping(src.add(offset), dst.add(offset), total_bytes - offset);
        }
    }
}

pub fn convert_bgra_to_rgba(src: &[u8], dst: &mut [u8], pixel_count: usize) {
    let required = pixel_count
        .checked_mul(4)
        .expect("pixel_count overflow when converting BGRA to RGBA");
    assert!(
        src.len() >= required,
        "BGRA source buffer too small: got {}, need at least {} bytes",
        src.len(),
        required
    );
    assert!(
        dst.len() >= required,
        "RGBA destination buffer too small: got {}, need at least {} bytes",
        dst.len(),
        required
    );
    unsafe {
        convert_bgra_to_rgba_unchecked(src.as_ptr(), dst.as_mut_ptr(), pixel_count);
    }
}

pub fn convert_bgra_to_rgba_opaque(src: &[u8], dst: &mut [u8], pixel_count: usize) {
    let required = pixel_count
        .checked_mul(4)
        .expect("pixel_count overflow when converting BGRA to RGBA");
    assert!(
        src.len() >= required,
        "BGRA source buffer too small: got {}, need at least {} bytes",
        src.len(),
        required
    );
    assert!(
        dst.len() >= required,
        "RGBA destination buffer too small: got {}, need at least {} bytes",
        dst.len(),
        required
    );
    unsafe {
        convert_bgra_to_rgba_opaque_unchecked(src.as_ptr(), dst.as_mut_ptr(), pixel_count);
    }
}

pub fn convert_f16_rgba_to_srgb(src: &[u8], dst: &mut [u8], pixel_count: usize) {
    let required_src = pixel_count
        .checked_mul(8)
        .expect("pixel_count overflow when converting RGBA16F to sRGB");
    let required_dst = pixel_count
        .checked_mul(4)
        .expect("pixel_count overflow when converting RGBA16F to sRGB");
    assert!(
        src.len() >= required_src,
        "RGBA16F source buffer too small: got {}, need at least {} bytes",
        src.len(),
        required_src
    );
    assert!(
        dst.len() >= required_dst,
        "RGBA destination buffer too small: got {}, need at least {} bytes",
        dst.len(),
        required_dst
    );
    unsafe {
        convert_f16_rgba_to_srgb_unchecked(src.as_ptr(), dst.as_mut_ptr(), pixel_count);
    }
}

pub fn convert_f16_rgba_to_srgb_opaque(src: &[u8], dst: &mut [u8], pixel_count: usize) {
    let required_src = pixel_count
        .checked_mul(8)
        .expect("pixel_count overflow when converting RGBA16F to opaque sRGB");
    let required_dst = pixel_count
        .checked_mul(4)
        .expect("pixel_count overflow when converting RGBA16F to opaque sRGB");
    assert!(
        src.len() >= required_src,
        "RGBA16F source buffer too small: got {}, need at least {} bytes",
        src.len(),
        required_src
    );
    assert!(
        dst.len() >= required_dst,
        "RGBA destination buffer too small: got {}, need at least {} bytes",
        dst.len(),
        required_dst
    );
    unsafe {
        convert_f16_rgba_to_srgb_opaque_unchecked(src.as_ptr(), dst.as_mut_ptr(), pixel_count);
    }
}

#[inline(always)]
fn bgra_kernel() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_kernel)
}

#[inline(always)]
fn bgra_opaque_kernel() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_opaque_kernel)
}

/// Non-temporal (streaming-store) variant of the BGRA kernel.
/// Falls back to the regular kernel on platforms without NT intrinsics.
#[inline(always)]
fn bgra_kernel_nt() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_kernel_nt)
}

#[inline(always)]
fn bgra_opaque_kernel_nt() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_opaque_kernel_nt)
}

#[inline(always)]
fn bgra_kernel_nt_nofence() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_kernel_nt_nofence)
}

#[inline(always)]
fn bgra_opaque_kernel_nt_nofence() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_bgra_opaque_kernel_nt_nofence)
}

/// Best-available F16->sRGB kernel (SIMD when possible, scalar fallback).
#[inline(always)]
fn f16_kernel() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_kernel)
}

#[inline(always)]
fn f16_opaque_kernel() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_opaque_kernel)
}

/// Non-temporal (streaming-store) variant of the F16->sRGB kernel.
/// Uses NT stores for the output to avoid cache pollution on large surfaces.
#[inline(always)]
fn f16_kernel_nt() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_kernel_nt)
}

#[inline(always)]
fn f16_opaque_kernel_nt() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_opaque_kernel_nt)
}

#[inline(always)]
fn f16_kernel_nt_nofence() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_kernel_nt_nofence)
}

#[inline(always)]
fn f16_opaque_kernel_nt_nofence() -> PixelKernel {
    static KERNEL: OnceLock<PixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_opaque_kernel_nt_nofence)
}

fn select_f16_kernel() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_unchecked
}

fn select_f16_opaque_kernel() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_opaque_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked
}

fn select_f16_kernel_nt() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_nt_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_nt_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_unchecked
}

fn select_f16_opaque_kernel_nt() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_nt_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_nt_opaque_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked
}

fn select_f16_kernel_nt_nofence() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_nt_nofence_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_nt_nofence_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_unchecked
}

fn select_f16_opaque_kernel_nt_nofence() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_avx512_nt_nofence_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            return simd_x86::convert_f16_rgba_to_srgb_f16c_nt_nofence_opaque_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked
}

#[inline(always)]
fn f16_hdr_prepared_kernel() -> HdrPreparedPixelKernel {
    static KERNEL: OnceLock<HdrPreparedPixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_hdr_prepared_kernel)
}

#[inline(always)]
fn f16_hdr_prepared_opaque_kernel() -> HdrPreparedPixelKernel {
    static KERNEL: OnceLock<HdrPreparedPixelKernel> = OnceLock::new();
    *KERNEL.get_or_init(select_f16_hdr_prepared_opaque_kernel)
}

fn select_f16_hdr_prepared_kernel() -> HdrPreparedPixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
            && std::arch::is_x86_feature_detected!("avx2")
        {
            if std::arch::is_x86_feature_detected!("fma") {
                return simd_x86::convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_unchecked;
            }
            return simd_x86::convert_f16_rgba_to_srgb_hdr_avx512_prepared_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            if std::arch::is_x86_feature_detected!("fma") {
                return simd_x86::convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_unchecked;
            }
            return simd_x86::convert_f16_rgba_to_srgb_hdr_f16c_prepared_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked
}

fn select_f16_hdr_prepared_opaque_kernel() -> HdrPreparedPixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
            && std::arch::is_x86_feature_detected!("f16c")
            && std::arch::is_x86_feature_detected!("avx2")
        {
            if std::arch::is_x86_feature_detected!("fma") {
                return simd_x86::convert_f16_rgba_to_srgb_hdr_avx512_fma_prepared_opaque_unchecked;
            }
            return simd_x86::convert_f16_rgba_to_srgb_hdr_avx512_prepared_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("f16c")
        {
            if std::arch::is_x86_feature_detected!("fma") {
                return simd_x86::convert_f16_rgba_to_srgb_hdr_f16c_fma_prepared_opaque_unchecked;
            }
            return simd_x86::convert_f16_rgba_to_srgb_hdr_f16c_prepared_opaque_unchecked;
        }
    }

    f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked
}

fn select_bgra_kernel() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_unchecked
}

fn select_bgra_opaque_kernel() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_opaque_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_opaque_unchecked
}

fn select_bgra_kernel_nt() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_nt_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_nt_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_nt_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_unchecked
}

fn select_bgra_opaque_kernel_nt() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_nt_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_nt_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_nt_opaque_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_opaque_unchecked
}

fn select_bgra_kernel_nt_nofence() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_nt_nofence_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_nt_nofence_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_nt_nofence_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_unchecked
}

fn select_bgra_opaque_kernel_nt_nofence() -> PixelKernel {
    #[cfg(target_arch = "x86_64")]
    {
        if std::arch::is_x86_feature_detected!("avx512f")
            && std::arch::is_x86_feature_detected!("avx512bw")
        {
            return simd_x86::convert_bgra_to_rgba_avx512_nt_nofence_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("avx2") {
            return simd_x86::convert_bgra_to_rgba_avx2_nt_nofence_opaque_unchecked;
        }
        if std::arch::is_x86_feature_detected!("ssse3") {
            return simd_x86::convert_bgra_to_rgba_ssse3_nt_nofence_opaque_unchecked;
        }
    }

    scalar::convert_bgra_to_rgba_scalar_opaque_unchecked
}

pub(crate) unsafe fn convert_bgra_to_rgba_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_unchecked_impl(src, dst, pixel_count, false);
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_unchecked_impl(src, dst, pixel_count, true);
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_opaque_serial_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        bgra_opaque_kernel()(src, dst, pixel_count);
    }
}

unsafe fn convert_bgra_to_rgba_unchecked_impl(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    if should_parallelize(
        pixel_count,
        BGRA_PARALLEL_MIN_PIXELS,
        BGRA_PARALLEL_MIN_CHUNK_PIXELS,
        BGRA_PARALLEL_MAX_WORKERS,
    ) {
        unsafe {
            convert_bgra_to_rgba_parallel(src, dst, pixel_count, force_opaque_alpha);
        }
        return;
    }
    unsafe {
        if force_opaque_alpha {
            bgra_opaque_kernel()(src, dst, pixel_count);
        } else {
            bgra_kernel()(src, dst, pixel_count);
        }
    }
}

/// Non-temporal BGRA->RGBA conversion optimised for the GDI capture path.
///
/// Differences from `convert_bgra_to_rgba_unchecked`:
///   1. Always uses streaming (NT) stores - the destination buffer will
///      not be read back before the next capture, so polluting the cache
///      with write-allocate traffic is pure waste.
///   2. Uses a lower parallelisation threshold so that 1080p captures
///      (~2 MP) reliably hit the multi-threaded path.
///
/// # Safety
///
/// * `src` and `dst` must not overlap.
/// * Both buffers must be at least `pixel_count * 4` bytes.
pub(crate) unsafe fn convert_bgra_to_rgba_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_nt_unchecked_impl(src, dst, pixel_count, false);
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_opaque_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_nt_unchecked_impl(src, dst, pixel_count, true);
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_opaque_nt_serial_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    let Some(nt_kernel) = bgra_nt_kernel_for_destination(dst as *const u8, true) else {
        unsafe {
            convert_bgra_to_rgba_opaque_serial_unchecked(src, dst, pixel_count);
        }
        return;
    };
    unsafe {
        nt_kernel(src, dst, pixel_count);
    }
}

unsafe fn convert_bgra_to_rgba_nt_unchecked_impl(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    let Some(nt_kernel) = bgra_nt_kernel_for_destination(dst as *const u8, force_opaque_alpha)
    else {
        unsafe {
            convert_bgra_to_rgba_unchecked_impl(src, dst, pixel_count, force_opaque_alpha);
        }
        return;
    };

    if should_parallelize(
        pixel_count,
        BGRA_NT_PARALLEL_MIN_PIXELS,
        BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS,
        BGRA_PARALLEL_MAX_WORKERS,
    ) {
        unsafe {
            convert_bgra_to_rgba_parallel_inner(
                src,
                dst,
                pixel_count,
                BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS,
                BGRA_PARALLEL_MAX_WORKERS,
                Some(nt_kernel),
                force_opaque_alpha,
            );
        }
        return;
    }
    // Serial path - still use NT stores since src != dst is guaranteed
    // by the caller and the working set far exceeds the cache.
    unsafe {
        nt_kernel(src, dst, pixel_count);
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_unchecked_impl(src, dst, pixel_count, false);
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_unchecked_impl(src, dst, pixel_count, true);
    }
}

unsafe fn convert_f16_rgba_to_srgb_unchecked_impl(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    if should_parallelize(
        pixel_count,
        F16_PARALLEL_MIN_PIXELS,
        F16_PARALLEL_MIN_CHUNK_PIXELS,
        F16_PARALLEL_MAX_WORKERS,
    ) {
        unsafe {
            convert_f16_rgba_to_srgb_parallel(src, dst, pixel_count, force_opaque_alpha);
        }
        return;
    }
    unsafe {
        if force_opaque_alpha {
            f16_opaque_kernel()(src, dst, pixel_count);
        } else {
            f16_kernel()(src, dst, pixel_count);
        }
    }
}

/// Non-temporal F16->sRGB conversion for non-overlapping buffers.
///
/// Uses the lower NT parallelisation thresholds since the destination
/// buffer won't be read back before the next capture.
///
/// # Safety
///
/// * `src` and `dst` must not overlap.
/// * `src` must be at least `pixel_count * 8` bytes.
/// * `dst` must be at least `pixel_count * 4` bytes.
pub(crate) unsafe fn convert_f16_rgba_to_srgb_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_nt_unchecked_impl(src, dst, pixel_count, false);
    }
}

pub(crate) unsafe fn convert_f16_rgba_to_srgb_opaque_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_f16_rgba_to_srgb_nt_unchecked_impl(src, dst, pixel_count, true);
    }
}

unsafe fn convert_f16_rgba_to_srgb_nt_unchecked_impl(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    if !nt_destination_is_aligned(dst as *const u8) {
        unsafe {
            convert_f16_rgba_to_srgb_unchecked_impl(src, dst, pixel_count, force_opaque_alpha);
        }
        return;
    }

    // F16 conversion is compute-heavy enough that NT stores don't help as
    // much as for BGRA (the bottleneck is ALU, not memory bandwidth), but
    // we still benefit from the lower parallelisation thresholds.
    if should_parallelize(
        pixel_count,
        BGRA_NT_PARALLEL_MIN_PIXELS,
        BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS,
        F16_PARALLEL_MAX_WORKERS,
    ) {
        unsafe {
            convert_f16_rgba_to_srgb_parallel_nt(src, dst, pixel_count, force_opaque_alpha);
        }
        return;
    }
    unsafe {
        if force_opaque_alpha {
            f16_opaque_kernel_nt()(src, dst, pixel_count);
        } else {
            f16_kernel_nt()(src, dst, pixel_count);
        }
    }
}

unsafe fn convert_f16_rgba_to_srgb_parallel_inner(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
    kernel: PixelKernel,
    force_opaque_alpha: bool,
) {
    let Some(chunk_pixels) = parallel_chunk_pixels(pixel_count, min_chunk_pixels, max_workers)
    else {
        unsafe { kernel(src, dst, pixel_count) };
        return;
    };

    let src_total = pixel_count.checked_mul(8).expect("pixel_count overflow");
    let dst_total = pixel_count.checked_mul(4).expect("pixel_count overflow");
    if parallel::ranges_overlap(src, src_total, dst, dst_total) {
        unsafe {
            if force_opaque_alpha {
                f16_opaque_kernel()(src, dst, pixel_count);
            } else {
                f16_kernel()(src, dst, pixel_count);
            }
        };
        return;
    }

    let chunk_count = pixel_count.div_ceil(chunk_pixels);
    let src_addr = src as usize;
    let dst_addr = dst as usize;

    use rayon::prelude::*;
    install_conversion_pool(CONVERSION_PARALLEL_MAX_WORKERS, || {
        (0..chunk_count)
            .into_par_iter()
            .for_each(|chunk_idx| unsafe {
                let start = chunk_idx * chunk_pixels;
                let len = (pixel_count - start).min(chunk_pixels);
                kernel(
                    (src_addr + start * 8) as *const u8,
                    (dst_addr + start * 4) as *mut u8,
                    len,
                );
            });
    });
}

unsafe fn convert_bgra_to_rgba_parallel(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    let nt_kernel = bgra_nt_kernel_for_destination(dst as *const u8, force_opaque_alpha);
    unsafe {
        convert_bgra_to_rgba_parallel_inner(
            src,
            dst,
            pixel_count,
            BGRA_PARALLEL_MIN_CHUNK_PIXELS,
            BGRA_PARALLEL_MAX_WORKERS,
            nt_kernel,
            force_opaque_alpha,
        );
    }
}

unsafe fn convert_bgra_to_rgba_parallel_inner(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
    nt_kernel: Option<PixelKernel>,
    force_opaque_alpha: bool,
) {
    let Some(chunk_pixels) = parallel_chunk_pixels(pixel_count, min_chunk_pixels, max_workers)
    else {
        unsafe {
            if let Some(kernel) = nt_kernel {
                kernel(src, dst, pixel_count);
            } else if force_opaque_alpha {
                bgra_opaque_kernel()(src, dst, pixel_count);
            } else {
                bgra_kernel()(src, dst, pixel_count);
            }
        };
        return;
    };

    let total_bytes = pixel_count
        .checked_mul(4)
        .expect("pixel_count overflow while converting BGRA to RGBA");
    if parallel::ranges_overlap(src, total_bytes, dst, total_bytes) {
        unsafe {
            if force_opaque_alpha {
                bgra_opaque_kernel()(src, dst, pixel_count)
            } else {
                bgra_kernel()(src, dst, pixel_count)
            }
        };
        return;
    }

    let chunk_count = pixel_count.div_ceil(chunk_pixels);
    // Use non-temporal stores in the parallel path - each chunk is large
    // enough that the destination lines won't be read back before the
    // full conversion completes, so bypassing the cache is a net win.
    let kernel = nt_kernel.unwrap_or_else(|| {
        if force_opaque_alpha {
            bgra_opaque_kernel()
        } else {
            bgra_kernel()
        }
    });
    let src_addr = src as usize;
    let dst_addr = dst as usize;

    use rayon::prelude::*;
    install_conversion_pool(max_workers, || {
        (0..chunk_count)
            .into_par_iter()
            .for_each(|chunk_idx| unsafe {
                let start = chunk_idx * chunk_pixels;
                let len = (pixel_count - start).min(chunk_pixels);
                kernel(
                    (src_addr + start * 4) as *const u8,
                    (dst_addr + start * 4) as *mut u8,
                    len,
                );
            });
    });
}

unsafe fn convert_f16_rgba_to_srgb_parallel(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    unsafe {
        convert_f16_rgba_to_srgb_parallel_inner(
            src,
            dst,
            pixel_count,
            F16_PARALLEL_MIN_CHUNK_PIXELS,
            F16_PARALLEL_MAX_WORKERS,
            if force_opaque_alpha {
                f16_opaque_kernel()
            } else {
                f16_kernel()
            },
            force_opaque_alpha,
        );
    }
}

unsafe fn convert_f16_rgba_to_srgb_parallel_nt(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
    force_opaque_alpha: bool,
) {
    unsafe {
        convert_f16_rgba_to_srgb_parallel_inner(
            src,
            dst,
            pixel_count,
            BGRA_NT_PARALLEL_MIN_CHUNK_PIXELS,
            F16_PARALLEL_MAX_WORKERS,
            if force_opaque_alpha {
                f16_opaque_kernel_nt()
            } else {
                f16_kernel_nt()
            },
            force_opaque_alpha,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use half::f16;

    fn build_hdr_f16_surface(width: usize, height: usize, src_pitch: usize) -> Vec<u8> {
        let row_bytes = width * 8;
        let src_len = src_pitch * (height - 1) + row_bytes;
        let mut src = vec![0u8; src_len];

        let pixel_count = width * height;
        for idx in 0..pixel_count {
            let t = idx as f32 / (pixel_count.saturating_sub(1).max(1)) as f32;
            let boost = if (idx & 0x3) == 0 {
                1.0 + 0.35 * t
            } else {
                2.0 + 5.0 * t
            };
            let r = (0.03 + 0.92 * t) * boost;
            let g = (0.02 + 0.76 * (1.0 - t)) * (0.8 * boost + 0.2);
            let b = (0.04 + 0.67 * (0.5 + 0.5 * t)) * (0.7 * boost + 0.3);
            let a = 0.45 + 0.55 * t;

            let y = idx / width;
            let x = idx % width;
            let off = y * src_pitch + x * 8;
            src[off..off + 2].copy_from_slice(&f16::from_f32(r.max(0.0)).to_bits().to_le_bytes());
            src[off + 2..off + 4]
                .copy_from_slice(&f16::from_f32(g.max(0.0)).to_bits().to_le_bytes());
            src[off + 4..off + 6]
                .copy_from_slice(&f16::from_f32(b.max(0.0)).to_bits().to_le_bytes());
            src[off + 6..off + 8]
                .copy_from_slice(&f16::from_f32(a.clamp(0.0, 1.0)).to_bits().to_le_bytes());
        }

        src
    }

    fn build_sdr_f16_surface(width: usize, height: usize, src_pitch: usize) -> Vec<u8> {
        let row_bytes = width * 8;
        let src_len = src_pitch * (height - 1) + row_bytes;
        let mut src = vec![0u8; src_len];

        let pixel_count = width * height;
        for idx in 0..pixel_count {
            let t = idx as f32 / (pixel_count.saturating_sub(1).max(1)) as f32;
            let wobble = ((idx % 23) as f32) / 23.0;
            let r = (0.05 + 0.80 * t + 0.10 * wobble).clamp(0.0, 1.0);
            let g = (0.15 + 0.65 * (1.0 - t) + 0.08 * wobble).clamp(0.0, 1.0);
            let b = (0.10 + 0.70 * (0.35 + 0.65 * t) + 0.06 * wobble).clamp(0.0, 1.0);
            let a = (0.10 + 0.85 * ((idx % 97) as f32 / 96.0)).clamp(0.0, 1.0);

            let y = idx / width;
            let x = idx % width;
            let off = y * src_pitch + x * 8;
            src[off..off + 2].copy_from_slice(&f16::from_f32(r).to_bits().to_le_bytes());
            src[off + 2..off + 4].copy_from_slice(&f16::from_f32(g).to_bits().to_le_bytes());
            src[off + 4..off + 6].copy_from_slice(&f16::from_f32(b).to_bits().to_le_bytes());
            src[off + 6..off + 8].copy_from_slice(&f16::from_f32(a).to_bits().to_le_bytes());
        }

        src
    }

    fn assert_rgba_within_lsb(actual: &[u8], expected: &[u8], pixel_count: usize, max_diff: u8) {
        for i in 0..pixel_count {
            let base = i * 4;
            assert!(
                actual[base].abs_diff(expected[base]) <= max_diff,
                "R differs at pixel {i}: actual={}, expected={}",
                actual[base],
                expected[base]
            );
            assert!(
                actual[base + 1].abs_diff(expected[base + 1]) <= max_diff,
                "G differs at pixel {i}: actual={}, expected={}",
                actual[base + 1],
                expected[base + 1]
            );
            assert!(
                actual[base + 2].abs_diff(expected[base + 2]) <= max_diff,
                "B differs at pixel {i}: actual={}, expected={}",
                actual[base + 2],
                expected[base + 2]
            );
            assert_eq!(
                actual[base + 3],
                expected[base + 3],
                "A differs at pixel {i}: actual={}, expected={}",
                actual[base + 3],
                expected[base + 3]
            );
        }
    }

    #[test]
    fn bgra_nt_fallback_handles_unaligned_destination() {
        let pixel_count = NT_STORE_MIN_PIXELS + 64;
        let mut src = vec![0u8; pixel_count * 4];
        for i in 0..pixel_count {
            let idx = i * 4;
            src[idx] = (i & 0xFF) as u8;
            src[idx + 1] = ((i >> 1) & 0xFF) as u8;
            src[idx + 2] = ((i >> 2) & 0xFF) as u8;
            src[idx + 3] = 0x2A;
        }

        let mut dst_storage = vec![0u8; pixel_count * 4 + 1];
        let dst_ptr = unsafe { dst_storage.as_mut_ptr().add(1) };

        unsafe {
            convert_bgra_to_rgba_nt_unchecked(src.as_ptr(), dst_ptr, pixel_count);
        }

        let dst = unsafe { std::slice::from_raw_parts(dst_ptr, pixel_count * 4) };
        for i in 0..pixel_count {
            let idx = i * 4;
            assert_eq!(dst[idx], src[idx + 2]);
            assert_eq!(dst[idx + 1], src[idx + 1]);
            assert_eq!(dst[idx + 2], src[idx]);
            assert_eq!(dst[idx + 3], 0x2A);
        }
    }

    #[test]
    fn convert_surface_to_rgba_handles_pitched_bgra() {
        let width = 7usize;
        let height = 5usize;
        let src_pitch = 36usize;
        let dst_pitch = width * 4;
        let src_row_bytes = width * 4;
        let src_len = src_pitch * (height - 1) + src_row_bytes;
        let dst_len = dst_pitch * (height - 1) + dst_pitch;

        let mut src = vec![0u8; src_len];
        for y in 0..height {
            for x in 0..width {
                let i = y * src_pitch + x * 4;
                src[i] = (x * 3 + y * 5) as u8; // B
                src[i + 1] = (x * 11 + y) as u8; // G
                src[i + 2] = (x + y * 13) as u8; // R
                src[i + 3] = 0x40 + (x as u8); // A
            }
        }

        let mut dst = vec![0u8; dst_len];
        convert_surface_to_rgba(
            SurfacePixelFormat::Bgra8,
            &src,
            src_pitch,
            &mut dst,
            dst_pitch,
            width,
            height,
            SurfaceConversionOptions::default(),
        );

        for y in 0..height {
            for x in 0..width {
                let src_i = y * src_pitch + x * 4;
                let dst_i = y * dst_pitch + x * 4;
                assert_eq!(dst[dst_i], src[src_i + 2]); // R
                assert_eq!(dst[dst_i + 1], src[src_i + 1]); // G
                assert_eq!(dst[dst_i + 2], src[src_i]); // B
                assert_eq!(dst[dst_i + 3], src[src_i + 3]); // A
            }
        }
    }

    #[test]
    fn convert_surface_to_rgba_force_opaque_alpha_overrides_bgra_source_alpha() {
        let width = 7usize;
        let height = 5usize;
        let src_pitch = 36usize;
        let dst_pitch = width * 4;
        let src_row_bytes = width * 4;
        let src_len = src_pitch * (height - 1) + src_row_bytes;
        let dst_len = dst_pitch * (height - 1) + dst_pitch;

        let mut src = vec![0u8; src_len];
        for y in 0..height {
            for x in 0..width {
                let i = y * src_pitch + x * 4;
                src[i] = (x * 3 + y * 5) as u8;
                src[i + 1] = (x * 11 + y) as u8;
                src[i + 2] = (x + y * 13) as u8;
                src[i + 3] = (0x10 + x as u8).min(0xFE);
            }
        }

        let mut dst = vec![0u8; dst_len];
        convert_surface_to_rgba(
            SurfacePixelFormat::Bgra8,
            &src,
            src_pitch,
            &mut dst,
            dst_pitch,
            width,
            height,
            SurfaceConversionOptions {
                force_opaque_alpha: true,
                ..SurfaceConversionOptions::default()
            },
        );

        for y in 0..height {
            for x in 0..width {
                let src_i = y * src_pitch + x * 4;
                let dst_i = y * dst_pitch + x * 4;
                assert_eq!(dst[dst_i], src[src_i + 2]);
                assert_eq!(dst[dst_i + 1], src[src_i + 1]);
                assert_eq!(dst[dst_i + 2], src[src_i]);
                assert_eq!(dst[dst_i + 3], u8::MAX);
            }
        }
    }

    #[test]
    fn convert_surface_to_bgra_preserves_bgra_channels_and_forces_alpha() {
        let src = [3, 5, 7, 11, 13, 17, 19, 23];
        let mut dst = [0u8; 8];
        convert_surface_to_rgba(
            SurfacePixelFormat::Bgra8,
            &src,
            8,
            &mut dst,
            8,
            2,
            1,
            SurfaceConversionOptions {
                force_opaque_alpha: true,
                output_pixel_format: CapturePixelFormat::Bgra8,
                ..SurfaceConversionOptions::default()
            },
        );

        assert_eq!(dst, [3, 5, 7, 255, 13, 17, 19, 255]);
    }

    #[test]
    fn convert_surface_to_bgra_swaps_rgba_channels() {
        let src = [7, 5, 3, 11, 19, 17, 13, 23];
        let mut dst = [0u8; 8];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba8,
            &src,
            8,
            &mut dst,
            8,
            2,
            1,
            SurfaceConversionOptions {
                output_pixel_format: CapturePixelFormat::Bgra8,
                ..SurfaceConversionOptions::default()
            },
        );

        assert_eq!(dst, [3, 5, 7, 11, 13, 17, 19, 23]);
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_bgra_opaque_conversion_vs_post_alpha() {
        use std::hint::black_box;
        use std::time::Instant;

        fn force_rgba_alpha_opaque_reference(bytes: &mut [u8]) {
            for alpha in bytes[3..].iter_mut().step_by(4) {
                *alpha = u8::MAX;
            }
        }

        const WIDTH: usize = 2560;
        const HEIGHT: usize = 1440;
        const PIXELS: usize = WIDTH * HEIGHT;
        const ITERATIONS: usize = 80;
        const ROUNDS: usize = 4;

        let mut src = vec![0u8; PIXELS * 4];
        for idx in 0..PIXELS {
            let base = idx * 4;
            src[base] = (idx as u8).wrapping_mul(13).wrapping_add(7);
            src[base + 1] = (idx as u8).wrapping_mul(29).wrapping_add(11);
            src[base + 2] = (idx as u8).wrapping_mul(47).wrapping_add(19);
            src[base + 3] = (idx as u8).wrapping_mul(61).wrapping_add(23);
        }

        let mut baseline = vec![0u8; PIXELS * 4];
        let mut optimized = vec![0u8; PIXELS * 4];
        unsafe {
            convert_bgra_to_rgba_unchecked(src.as_ptr(), baseline.as_mut_ptr(), PIXELS);
            force_rgba_alpha_opaque_reference(&mut baseline);
            convert_bgra_to_rgba_opaque_unchecked(src.as_ptr(), optimized.as_mut_ptr(), PIXELS);
        }
        assert_eq!(baseline, optimized);

        let mut best_baseline = std::time::Duration::MAX;
        let mut best_optimized = std::time::Duration::MAX;

        for _round in 0..ROUNDS {
            let mut baseline_dst = vec![0u8; PIXELS * 4];
            let mut baseline_checksum = 0u64;
            let baseline_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    convert_bgra_to_rgba_unchecked(src.as_ptr(), baseline_dst.as_mut_ptr(), PIXELS);
                }
                force_rgba_alpha_opaque_reference(&mut baseline_dst);
                baseline_checksum = baseline_checksum
                    .wrapping_add(baseline_dst[(iter * 257) % baseline_dst.len()] as u64);
            }
            black_box(baseline_checksum);
            best_baseline = best_baseline.min(baseline_start.elapsed());

            let mut optimized_dst = vec![0u8; PIXELS * 4];
            let mut optimized_checksum = 0u64;
            let optimized_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    convert_bgra_to_rgba_opaque_unchecked(
                        src.as_ptr(),
                        optimized_dst.as_mut_ptr(),
                        PIXELS,
                    );
                }
                optimized_checksum = optimized_checksum
                    .wrapping_add(optimized_dst[(iter * 193) % optimized_dst.len()] as u64);
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

        println!(
            "bgra opaque-convert benchmark: baseline={baseline_ms:.3} ms optimized={optimized_ms:.3} ms speedup={speedup:.2}x"
        );
        assert!(
            optimized_ms < baseline_ms,
            "opaque fused conversion failed to beat baseline: baseline={baseline_ms:.3}ms optimized={optimized_ms:.3}ms ({speedup:.2}x)"
        );
    }

    #[test]
    fn convert_f16_rgba_to_srgb_opaque_matches_post_alpha_reference() {
        fn force_rgba_alpha_opaque_reference(bytes: &mut [u8]) {
            for alpha in bytes[3..].iter_mut().step_by(4) {
                *alpha = u8::MAX;
            }
        }

        let width = 257usize;
        let height = 33usize;
        let pixel_count = width * height;
        let src = build_sdr_f16_surface(width, height, width * 8);
        let mut baseline = vec![0u8; pixel_count * 4];
        let mut optimized = vec![0u8; pixel_count * 4];

        unsafe {
            convert_f16_rgba_to_srgb_unchecked(src.as_ptr(), baseline.as_mut_ptr(), pixel_count);
            convert_f16_rgba_to_srgb_opaque_unchecked(
                src.as_ptr(),
                optimized.as_mut_ptr(),
                pixel_count,
            );
        }
        force_rgba_alpha_opaque_reference(&mut baseline);
        assert_eq!(baseline, optimized);
    }

    #[test]
    fn convert_f16_hdr_opaque_kernel_matches_post_alpha_reference() {
        fn force_rgba_alpha_opaque_reference(bytes: &mut [u8]) {
            for alpha in bytes[3..].iter_mut().step_by(4) {
                *alpha = u8::MAX;
            }
        }

        let width = 321usize;
        let height = 17usize;
        let pixel_count = width * height;
        let src = build_hdr_f16_surface(width, height, width * 8);
        let params = HdrFrameContext {
            sdr_white_nits: 160.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: true,
            ..HdrFrameContext::default()
        };
        let prepared = prepare_hdr_context_cached(params.sanitized());
        let mut baseline = vec![0u8; pixel_count * 4];
        let mut optimized = vec![0u8; pixel_count * 4];

        unsafe {
            f16_hdr_prepared_kernel()(src.as_ptr(), baseline.as_mut_ptr(), pixel_count, &prepared);
            f16_hdr_prepared_opaque_kernel()(
                src.as_ptr(),
                optimized.as_mut_ptr(),
                pixel_count,
                &prepared,
            );
        }
        force_rgba_alpha_opaque_reference(&mut baseline);
        assert_eq!(baseline, optimized);
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_f16_opaque_conversion_vs_post_alpha() {
        use std::hint::black_box;
        use std::time::Instant;

        fn force_rgba_alpha_opaque_reference(bytes: &mut [u8]) {
            for alpha in bytes[3..].iter_mut().step_by(4) {
                *alpha = u8::MAX;
            }
        }

        const WIDTH: usize = 2560;
        const HEIGHT: usize = 1440;
        const PIXELS: usize = WIDTH * HEIGHT;
        const ITERATIONS: usize = 32;
        const ROUNDS: usize = 4;

        let src = build_sdr_f16_surface(WIDTH, HEIGHT, WIDTH * 8);
        let mut baseline = vec![0u8; PIXELS * 4];
        let mut optimized = vec![0u8; PIXELS * 4];
        unsafe {
            convert_f16_rgba_to_srgb_unchecked(src.as_ptr(), baseline.as_mut_ptr(), PIXELS);
            force_rgba_alpha_opaque_reference(&mut baseline);
            convert_f16_rgba_to_srgb_opaque_unchecked(src.as_ptr(), optimized.as_mut_ptr(), PIXELS);
        }
        assert_eq!(baseline, optimized);

        let mut best_baseline = std::time::Duration::MAX;
        let mut best_optimized = std::time::Duration::MAX;

        for _round in 0..ROUNDS {
            let mut baseline_dst = vec![0u8; PIXELS * 4];
            let mut baseline_checksum = 0u64;
            let baseline_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    convert_f16_rgba_to_srgb_unchecked(
                        src.as_ptr(),
                        baseline_dst.as_mut_ptr(),
                        PIXELS,
                    );
                }
                force_rgba_alpha_opaque_reference(&mut baseline_dst);
                baseline_checksum = baseline_checksum
                    .wrapping_add(baseline_dst[(iter * 257) % baseline_dst.len()] as u64);
            }
            black_box(baseline_checksum);
            best_baseline = best_baseline.min(baseline_start.elapsed());

            let mut optimized_dst = vec![0u8; PIXELS * 4];
            let mut optimized_checksum = 0u64;
            let optimized_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    convert_f16_rgba_to_srgb_opaque_unchecked(
                        src.as_ptr(),
                        optimized_dst.as_mut_ptr(),
                        PIXELS,
                    );
                }
                optimized_checksum = optimized_checksum
                    .wrapping_add(optimized_dst[(iter * 193) % optimized_dst.len()] as u64);
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

        println!(
            "f16 opaque-convert benchmark: baseline={baseline_ms:.3} ms optimized={optimized_ms:.3} ms speedup={speedup:.2}x"
        );
        assert!(
            optimized_ms < baseline_ms,
            "opaque fused conversion failed to beat baseline: baseline={baseline_ms:.3}ms optimized={optimized_ms:.3}ms ({speedup:.2}x)"
        );
    }

    #[test]
    #[ignore = "performance benchmark guard; run explicitly with --ignored --nocapture"]
    fn bench_f16_hdr_opaque_conversion_vs_post_alpha() {
        use std::hint::black_box;
        use std::time::Instant;

        fn force_rgba_alpha_opaque_reference(bytes: &mut [u8]) {
            for alpha in bytes[3..].iter_mut().step_by(4) {
                *alpha = u8::MAX;
            }
        }

        const WIDTH: usize = 2560;
        const HEIGHT: usize = 1440;
        const PIXELS: usize = WIDTH * HEIGHT;
        const ITERATIONS: usize = 24;
        const ROUNDS: usize = 4;

        let src = build_hdr_f16_surface(WIDTH, HEIGHT, WIDTH * 8);
        let params = HdrFrameContext {
            sdr_white_nits: 160.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: true,
            ..HdrFrameContext::default()
        };
        let prepared = prepare_hdr_context_cached(params.sanitized());
        let mut baseline = vec![0u8; PIXELS * 4];
        let mut optimized = vec![0u8; PIXELS * 4];
        unsafe {
            f16_hdr_prepared_kernel()(src.as_ptr(), baseline.as_mut_ptr(), PIXELS, &prepared);
            force_rgba_alpha_opaque_reference(&mut baseline);
            f16_hdr_prepared_opaque_kernel()(
                src.as_ptr(),
                optimized.as_mut_ptr(),
                PIXELS,
                &prepared,
            );
        }
        assert_eq!(baseline, optimized);

        let mut best_baseline = std::time::Duration::MAX;
        let mut best_optimized = std::time::Duration::MAX;

        for _round in 0..ROUNDS {
            let mut baseline_dst = vec![0u8; PIXELS * 4];
            let mut baseline_checksum = 0u64;
            let baseline_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    f16_hdr_prepared_kernel()(
                        src.as_ptr(),
                        baseline_dst.as_mut_ptr(),
                        PIXELS,
                        &prepared,
                    );
                }
                force_rgba_alpha_opaque_reference(&mut baseline_dst);
                baseline_checksum = baseline_checksum
                    .wrapping_add(baseline_dst[(iter * 257) % baseline_dst.len()] as u64);
            }
            black_box(baseline_checksum);
            best_baseline = best_baseline.min(baseline_start.elapsed());

            let mut optimized_dst = vec![0u8; PIXELS * 4];
            let mut optimized_checksum = 0u64;
            let optimized_start = Instant::now();
            for iter in 0..ITERATIONS {
                unsafe {
                    f16_hdr_prepared_opaque_kernel()(
                        src.as_ptr(),
                        optimized_dst.as_mut_ptr(),
                        PIXELS,
                        &prepared,
                    );
                }
                optimized_checksum = optimized_checksum
                    .wrapping_add(optimized_dst[(iter * 193) % optimized_dst.len()] as u64);
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

        println!(
            "f16 hdr opaque-convert benchmark: baseline={baseline_ms:.3} ms optimized={optimized_ms:.3} ms speedup={speedup:.2}x"
        );
        assert!(
            optimized_ms < baseline_ms,
            "opaque fused HDR conversion failed to beat baseline: baseline={baseline_ms:.3}ms optimized={optimized_ms:.3}ms ({speedup:.2}x)"
        );
    }

    #[test]
    fn convert_surface_to_rgba_handles_pitched_rgba_passthrough() {
        let width = 9usize;
        let height = 4usize;
        let src_pitch = 44usize;
        let dst_pitch = 44usize;
        let row_bytes = width * 4;
        let len = src_pitch * (height - 1) + row_bytes;

        let mut src = vec![0u8; len];
        for y in 0..height {
            for x in 0..width {
                let i = y * src_pitch + x * 4;
                src[i] = (x + y) as u8;
                src[i + 1] = (x * 2 + y * 3) as u8;
                src[i + 2] = (x * 7 + y * 5) as u8;
                src[i + 3] = 0x80;
            }
        }

        let mut dst = vec![0u8; len];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba8,
            &src,
            src_pitch,
            &mut dst,
            dst_pitch,
            width,
            height,
            SurfaceConversionOptions::default(),
        );

        for y in 0..height {
            let src_row = &src[y * src_pitch..y * src_pitch + row_bytes];
            let dst_row = &dst[y * dst_pitch..y * dst_pitch + row_bytes];
            assert_eq!(dst_row, src_row);
        }
    }

    #[test]
    fn convert_surface_to_rgba_handles_pitched_rgba16f() {
        let width = 7usize;
        let height = 3usize;
        let src_pitch = 64usize;
        let dst_pitch = 40usize;
        let src_row_bytes = width * 8;
        let dst_row_bytes = width * 4;
        let src_len = src_pitch * (height - 1) + src_row_bytes;
        let dst_len = dst_pitch * (height - 1) + dst_row_bytes;

        let mut src = vec![0u8; src_len];
        for (idx, byte) in src.iter_mut().enumerate() {
            *byte = (idx.wrapping_mul(17).wrapping_add(31) & 0xFF) as u8;
        }

        let mut dst = vec![0xCDu8; dst_len];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut dst,
            dst_pitch,
            width,
            height,
            SurfaceConversionOptions::default(),
        );

        let mut expected = vec![0xCDu8; dst_len];
        for y in 0..height {
            let src_row = &src[y * src_pitch..y * src_pitch + src_row_bytes];
            let dst_row = &mut expected[y * dst_pitch..y * dst_pitch + dst_row_bytes];
            convert_row_to_rgba_with_options(
                SurfacePixelFormat::Rgba16Float,
                src_row,
                dst_row,
                width,
                SurfaceConversionOptions::default(),
            );
        }

        for y in 0..height {
            let row_start = y * dst_pitch;
            let row_end = row_start + dst_row_bytes;
            assert_eq!(&dst[row_start..row_end], &expected[row_start..row_end]);

            let pad_end = ((y + 1) * dst_pitch).min(dst.len());
            if row_end < pad_end {
                assert!(dst[row_end..pad_end].iter().all(|&v| v == 0xCD));
            }
        }
    }

    #[test]
    fn rgba16f_sdr_surface_and_row_converter_produce_bgra() {
        let width = 7usize;
        let height = 3usize;
        let src_pitch = 64usize;
        let dst_pitch = 36usize;
        let src = build_sdr_f16_surface(width, height, src_pitch);
        let mut expected_rgba = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut expected_rgba,
            dst_pitch,
            width,
            height,
            SurfaceConversionOptions::default(),
        );

        let options = SurfaceConversionOptions {
            output_pixel_format: CapturePixelFormat::Bgra8,
            ..SurfaceConversionOptions::default()
        };
        let mut actual = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut actual,
            dst_pitch,
            width,
            height,
            options,
        );

        let mut row_actual = vec![0u8; dst_pitch * height];
        let converter = SurfaceRowConverter::new(SurfacePixelFormat::Rgba16Float, options);
        unsafe {
            converter.convert_rows_unchecked(
                src.as_ptr(),
                src_pitch,
                row_actual.as_mut_ptr(),
                dst_pitch,
                width,
                height,
            );
        }

        for y in 0..height {
            for x in 0..width {
                let index = y * dst_pitch + x * 4;
                assert_eq!(actual[index], expected_rgba[index + 2]);
                assert_eq!(actual[index + 1], expected_rgba[index + 1]);
                assert_eq!(actual[index + 2], expected_rgba[index]);
                assert_eq!(actual[index + 3], expected_rgba[index + 3]);
            }
        }
        assert_eq!(row_actual, actual);
    }

    #[test]
    fn rgba16f_hdr_surface_and_row_converter_produce_bgra() {
        let width = 11usize;
        let height = 4usize;
        let src_pitch = 96usize;
        let dst_pitch = 52usize;
        let src = build_hdr_f16_surface(width, height, src_pitch);
        let params = HdrFrameContext {
            sdr_white_nits: 160.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: true,
            ..HdrFrameContext::default()
        };
        let rgba_options = SurfaceConversionOptions {
            hdr_to_sdr: Some(params),
            ..SurfaceConversionOptions::default()
        };
        let mut expected_rgba = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut expected_rgba,
            dst_pitch,
            width,
            height,
            rgba_options,
        );

        let bgra_options = SurfaceConversionOptions {
            output_pixel_format: CapturePixelFormat::Bgra8,
            ..rgba_options
        };
        let mut actual = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut actual,
            dst_pitch,
            width,
            height,
            bgra_options,
        );

        let mut row_actual = vec![0u8; dst_pitch * height];
        let converter = SurfaceRowConverter::new(SurfacePixelFormat::Rgba16Float, bgra_options);
        unsafe {
            converter.convert_rows_unchecked(
                src.as_ptr(),
                src_pitch,
                row_actual.as_mut_ptr(),
                dst_pitch,
                width,
                height,
            );
        }

        for y in 0..height {
            for x in 0..width {
                let index = y * dst_pitch + x * 4;
                assert_eq!(actual[index], expected_rgba[index + 2]);
                assert_eq!(actual[index + 1], expected_rgba[index + 1]);
                assert_eq!(actual[index + 2], expected_rgba[index]);
                assert_eq!(actual[index + 3], expected_rgba[index + 3]);
            }
        }
        assert_eq!(row_actual, actual);
    }

    #[test]
    fn hdr_contiguous_surface_matches_serial_row_baseline_lut_within_1_lsb() {
        let width = 1024usize;
        let height = 512usize;
        let src_pitch = width * 8;
        let dst_pitch = width * 4;
        let src = build_hdr_f16_surface(width, height, src_pitch);
        let pixel_count = width * height;
        let params = HdrFrameContext {
            sdr_white_nits: 160.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: true,
            ..HdrFrameContext::default()
        };
        let options = SurfaceConversionOptions {
            hdr_to_sdr: Some(params),
            ..SurfaceConversionOptions::default()
        };

        let mut actual = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut actual,
            dst_pitch,
            width,
            height,
            options,
        );

        let mut expected = vec![0u8; dst_pitch * height];
        let converter = SurfaceRowConverter::new(SurfacePixelFormat::Rgba16Float, options);
        unsafe {
            converter.convert_rows_unchecked(
                src.as_ptr(),
                src_pitch,
                expected.as_mut_ptr(),
                dst_pitch,
                width,
                height,
            );
        }

        assert_rgba_within_lsb(&actual, &expected, pixel_count, 1);
    }

    #[test]
    fn hdr_contiguous_surface_matches_serial_row_baseline_precise_within_1_lsb() {
        let width = 1024usize;
        let height = 512usize;
        let src_pitch = width * 8;
        let dst_pitch = width * 4;
        let src = build_hdr_f16_surface(width, height, src_pitch);
        let pixel_count = width * height;
        let params = HdrFrameContext {
            sdr_white_nits: 160.0,
            hdr_peak_nits: 1000.0,
            tonemap_use_lut: false,
            ..HdrFrameContext::default()
        };
        let options = SurfaceConversionOptions {
            hdr_to_sdr: Some(params),
            ..SurfaceConversionOptions::default()
        };

        let mut actual = vec![0u8; dst_pitch * height];
        convert_surface_to_rgba(
            SurfacePixelFormat::Rgba16Float,
            &src,
            src_pitch,
            &mut actual,
            dst_pitch,
            width,
            height,
            options,
        );

        let mut expected = vec![0u8; dst_pitch * height];
        let converter = SurfaceRowConverter::new(SurfacePixelFormat::Rgba16Float, options);
        unsafe {
            converter.convert_rows_unchecked(
                src.as_ptr(),
                src_pitch,
                expected.as_mut_ptr(),
                dst_pitch,
                width,
                height,
            );
        }

        assert_rgba_within_lsb(&actual, &expected, pixel_count, 1);
    }
}

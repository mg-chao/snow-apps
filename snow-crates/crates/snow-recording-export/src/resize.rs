//! Shared nearest-neighbor pixel selection for live recording and export.
use rayon::prelude::*;
use std::ptr;

#[derive(Clone, Debug)]
pub struct NearestResizePlan {
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
    src_x_byte_offsets: Vec<usize>,
    src_row_byte_offsets: Vec<usize>,
}

impl NearestResizePlan {
    /// Returns whether this plan selects pixels for the supplied geometry.
    pub fn matches(&self, source: (u32, u32), output: (u32, u32)) -> bool {
        (self.src_w, self.src_h) == source && (self.dst_w, self.dst_h) == output
    }

    /// Resize tightly packed RGBA pixels into reusable storage, without row workers.
    /// Panics when either slice does not match the plan dimensions.
    pub fn resize_into(&self, source: &[u8], output: &mut [u8]) {
        self.resize_with_pool(source, output, None);
    }

    /// Use an explicitly bounded row pool while preserving the same selected pixels.
    pub fn resize_into_with_pool(
        &self,
        source: &[u8],
        output: &mut [u8],
        pool: &rayon::ThreadPool,
    ) {
        self.resize_with_pool(source, output, Some(pool));
    }

    fn resize_with_pool(&self, source: &[u8], output: &mut [u8], pool: Option<&rayon::ThreadPool>) {
        assert_eq!(source.len(), self.src_w as usize * self.src_h as usize * 4);
        assert_eq!(output.len(), self.dst_w as usize * self.dst_h as usize * 4);
        resize_rgba_fast_into(
            source,
            self.src_w,
            self.src_h,
            self.dst_w,
            self.dst_h,
            Some(self),
            output,
            pool,
        );
    }

    pub fn new(src_w: u32, src_h: u32, dst_w: u32, dst_h: u32) -> Self {
        let src_w_usize = src_w.max(1) as usize;
        let src_row_bytes = src_w_usize * 4;
        let src_x_byte_offsets = (0..dst_w.max(1))
            .map(|x| {
                let sx = ((x as u64 * src_w.max(1) as u64) / dst_w.max(1) as u64) as usize;
                sx * 4
            })
            .collect();
        let src_row_byte_offsets = (0..dst_h.max(1))
            .map(|y| {
                let sy = ((y as u64 * src_h.max(1) as u64) / dst_h.max(1) as u64) as usize;
                sy * src_row_bytes
            })
            .collect();

        Self {
            src_w: src_w.max(1),
            src_h: src_h.max(1),
            dst_w: dst_w.max(1),
            dst_h: dst_h.max(1),
            src_x_byte_offsets,
            src_row_byte_offsets,
        }
    }
}

pub(crate) fn resize_rgba_fast_into(
    src: &[u8],
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
    plan: Option<&NearestResizePlan>,
    out: &mut [u8],
    process_pool: Option<&rayon::ThreadPool>,
) {
    if src_w == dst_w && src_h == dst_h {
        out.copy_from_slice(src);
        return;
    }

    let expected = dst_w.max(1) as usize * dst_h.max(1) as usize * 4;
    debug_assert_eq!(out.len(), expected);

    if let Some(plan) = plan
        && plan.src_w == src_w.max(1)
        && plan.src_h == src_h.max(1)
        && plan.dst_w == dst_w.max(1)
        && plan.dst_h == dst_h.max(1)
    {
        resize_rgba_with_plan(src, plan, out, process_pool);
        return;
    }

    resize_rgba_scalar(src, src_w, src_h, dst_w, dst_h, out);
}

fn resize_rgba_with_plan(
    src: &[u8],
    plan: &NearestResizePlan,
    out: &mut [u8],
    process_pool: Option<&rayon::ThreadPool>,
) {
    assert_eq!(src.len(), plan.src_w as usize * plan.src_h as usize * 4);
    assert_eq!(out.len(), plan.dst_w as usize * plan.dst_h as usize * 4);
    let dst_row_bytes = plan.dst_w as usize * 4;
    let should_parallel = process_pool.is_some()
        && (plan.dst_w as usize * plan.dst_h as usize) >= 1_000_000
        && plan.dst_h >= 256;
    if should_parallel {
        let pool = process_pool.expect("checked Some above");
        pool.install(|| {
            out.par_chunks_exact_mut(dst_row_bytes)
                .enumerate()
                .for_each(|(y, row)| {
                    let src_row = plan.src_row_byte_offsets[y];
                    let row_u32 = row.as_mut_ptr() as *mut u32;
                    for (dst_x, src_x) in plan.src_x_byte_offsets.iter().enumerate() {
                        let src_offset = src_row + *src_x;
                        // SAFETY:
                        // - Source and destination pixel addresses are valid by plan construction.
                        // - Each parallel worker owns disjoint `row` slices.
                        unsafe {
                            let pixel =
                                ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                            ptr::write_unaligned(row_u32.add(dst_x), pixel);
                        }
                    }
                });
        });
        return;
    }

    for (y, row) in out.chunks_exact_mut(dst_row_bytes).enumerate() {
        let src_row = plan.src_row_byte_offsets[y];
        let row_u32 = row.as_mut_ptr() as *mut u32;
        for (dst_x, src_x) in plan.src_x_byte_offsets.iter().enumerate() {
            let src_offset = src_row + *src_x;
            // SAFETY:
            // - Source and destination pixel addresses are valid by plan construction.
            unsafe {
                let pixel = ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                ptr::write_unaligned(row_u32.add(dst_x), pixel);
            }
        }
    }
}

fn resize_rgba_scalar(src: &[u8], src_w: u32, src_h: u32, dst_w: u32, dst_h: u32, out: &mut [u8]) {
    let dst_row_bytes = dst_w.max(1) as usize * 4;
    for (y, row) in out.chunks_exact_mut(dst_row_bytes).enumerate() {
        let sy = ((y as u64 * src_h.max(1) as u64) / dst_h.max(1) as u64) as usize;
        let src_row = sy * src_w.max(1) as usize * 4;
        let row_u32 = row.as_mut_ptr() as *mut u32;
        for x in 0..dst_w.max(1) as usize {
            let sx = ((x as u64 * src_w.max(1) as u64) / dst_w.max(1) as u64) as usize;
            let src_offset = src_row + sx * 4;
            // SAFETY:
            // - Source and destination pixel addresses are valid by loop bounds.
            unsafe {
                let pixel = ptr::read_unaligned(src.as_ptr().add(src_offset) as *const u32);
                ptr::write_unaligned(row_u32.add(x), pixel);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn planned_pixels_match_integer_nearest_neighbor_for_all_geometries() {
        for (sw, sh, dw, dh) in [
            (4, 4, 4, 4),
            (17, 11, 7, 5),
            (11, 17, 5, 7),
            (1, 1, 3, 5),
            (64, 36, 32, 18),
            (9, 7, 1, 1),
        ] {
            let source: Vec<_> = (0..sw * sh * 4).map(|v| (v % 251) as u8).collect();
            let mut output = vec![0; (dw * dh * 4) as usize];
            let plan = NearestResizePlan::new(sw, sh, dw, dh);
            plan.resize_into(&source, &mut output);
            for y in 0..dh {
                for x in 0..dw {
                    let source_index = ((y * sh / dh) * sw + x * sw / dw) as usize * 4;
                    let target = (y * dw + x) as usize * 4;
                    assert_eq!(
                        &output[target..target + 4],
                        &source[source_index..source_index + 4]
                    );
                }
            }
            assert!(plan.matches((sw, sh), (dw, dh)));
            assert!(!plan.matches((sw + 1, sh), (dw, dh)));
            let pointer = output.as_ptr();
            plan.resize_into(&source, &mut output);
            assert_eq!(output.as_ptr(), pointer);
        }
    }

    #[test]
    #[should_panic]
    fn short_source_is_rejected_before_unchecked_pixel_access() {
        NearestResizePlan::new(4, 4, 2, 2).resize_into(&[0; 4], &mut [0; 16]);
    }

    #[test]
    fn bounded_row_workers_preserve_pixels_for_large_and_fallback_geometries() {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(2)
            .build()
            .unwrap();
        for (sw, sh, dw, dh) in [
            (3840, 2160, 1920, 1080),
            (2160, 3840, 1080, 1920),
            (1920, 1080, 1920, 1080),
            (1919, 1079, 959, 539),
        ] {
            let source: Vec<_> = (0..sw * sh * 4).map(|value| (value % 251) as u8).collect();
            let plan = NearestResizePlan::new(sw, sh, dw, dh);
            let mut serial = vec![0; (dw * dh * 4) as usize];
            let mut parallel = vec![165; serial.len()];
            plan.resize_into(&source, &mut serial);
            plan.resize_into_with_pool(&source, &mut parallel, &pool);
            assert_eq!(parallel, serial);
        }
    }
}

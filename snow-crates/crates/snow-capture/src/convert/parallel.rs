use std::sync::OnceLock;

const PARALLEL_CHUNK_ALIGNMENT_PIXELS: usize = 256;
static POOL: OnceLock<Option<rayon::ThreadPool>> = OnceLock::new();

#[cfg(test)]
pub(crate) fn pool_initialized() -> bool {
    POOL.get().is_some()
}

/// Pre-initialize the conversion thread pool so the first capture doesn't
/// pay the pool-creation cost (~10-50 ms).  Safe to call multiple times;
/// only the first call has any effect.
pub(crate) fn warmup_pool(max_workers: usize) {
    // Force the shared pool to initialize by running a trivial no-op job.
    install_conversion_pool(max_workers, || {});
}

#[inline(always)]
pub(crate) fn should_parallelize(
    pixel_count: usize,
    min_pixels: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
) -> bool {
    let workers = conversion_workers(max_workers);
    if workers <= 1 {
        return false;
    }
    let min_chunk_total = min_chunk_pixels.saturating_mul(workers);
    pixel_count >= min_pixels.max(min_chunk_total)
}

#[inline(always)]
pub(crate) fn parallel_chunk_pixels(
    pixel_count: usize,
    min_chunk_pixels: usize,
    max_workers: usize,
) -> Option<usize> {
    let alignment = PARALLEL_CHUNK_ALIGNMENT_PIXELS.max(1);
    let workers = conversion_workers(max_workers);
    let mut chunk_pixels = pixel_count / workers;

    if chunk_pixels < min_chunk_pixels {
        return None;
    }

    chunk_pixels -= chunk_pixels % alignment;
    if chunk_pixels == 0 || pixel_count.div_ceil(chunk_pixels) < 2 {
        return None;
    }

    Some(chunk_pixels)
}

#[inline(always)]
pub(crate) fn ranges_overlap(src: *const u8, src_len: usize, dst: *mut u8, dst_len: usize) -> bool {
    let src_start = src as usize;
    let dst_start = dst as usize;

    let Some(src_end) = src_start.checked_add(src_len) else {
        return true;
    };
    let Some(dst_end) = dst_start.checked_add(dst_len) else {
        return true;
    };

    src_start < dst_end && dst_start < src_end
}

#[inline]
pub(crate) fn conversion_workers(max_workers: usize) -> usize {
    static WORKERS: OnceLock<usize> = OnceLock::new();
    (*WORKERS.get_or_init(detect_conversion_worker_limit)).min(max_workers.max(1))
}

#[inline]
fn detect_conversion_worker_limit() -> usize {
    // Prefer physical cores so HDR/SDR CPU conversion doesn't over-subscribe
    // hyper-threads. Clamp to the currently available logical parallelism so
    // process affinity / cgroup limits are still respected.
    let logical = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1)
        .max(1);
    let physical = num_cpus::get_physical();
    if physical == 0 {
        logical
    } else {
        physical.min(logical).max(1)
    }
}

#[inline]
pub(crate) fn install_conversion_pool<F>(max_workers: usize, job: F)
where
    F: FnOnce() + Send,
{
    if let Some(pool) = POOL
        .get_or_init(|| {
            let workers = conversion_workers(max_workers);
            if workers <= 1 {
                return None;
            }
            rayon::ThreadPoolBuilder::new()
                .num_threads(workers)
                .build()
                .ok()
        })
        .as_ref()
    {
        pool.install(job);
    } else {
        job();
    }
}

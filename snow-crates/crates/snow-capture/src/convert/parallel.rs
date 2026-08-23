use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

const PARALLEL_CHUNK_ALIGNMENT_PIXELS: usize = 256;

/// Pre-initialize the conversion thread pool so the first capture doesn't
/// pay the pool-creation cost (~10-50 ms). Safe to call multiple times; a
/// released pool is recreated on demand.
pub(crate) fn warmup_pool(max_workers: usize) {
    install_conversion_pool(max_workers, || {});
}

/// Drop the process-wide reference to the conversion pool.
///
/// Work that has already acquired the pool keeps it alive until that work
/// completes. A later conversion creates a fresh pool on demand.
pub(crate) fn release_pool() {
    conversion_pool().release();
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
    let workers = conversion_workers(max_workers);
    let pool = conversion_pool().acquire(workers);

    if let Some(pool) = pool {
        pool.install(job);
    } else {
        job();
    }
}

struct ConversionPool {
    pool: Mutex<Option<Arc<rayon::ThreadPool>>>,
}

impl ConversionPool {
    const fn new() -> Self {
        Self {
            pool: Mutex::new(None),
        }
    }

    fn acquire(&self, workers: usize) -> Option<Arc<rayon::ThreadPool>> {
        if workers <= 1 {
            return None;
        }

        let mut pool = self.lock();
        if pool.is_none() {
            *pool = rayon::ThreadPoolBuilder::new()
                .num_threads(workers)
                .build()
                .ok()
                .map(Arc::new);
        }
        pool.clone()
    }

    fn release(&self) {
        // Drop outside the mutex: ThreadPool teardown can wait for its worker
        // threads, and pool acquisition must never be blocked by that wait.
        let pool = self.lock().take();
        drop(pool);
    }

    fn lock(&self) -> MutexGuard<'_, Option<Arc<rayon::ThreadPool>>> {
        self.pool
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }
}

fn conversion_pool() -> &'static ConversionPool {
    static POOL: ConversionPool = ConversionPool::new();
    &POOL
}

#[cfg(test)]
mod tests {
    use super::ConversionPool;
    use std::sync::Arc;

    #[test]
    fn release_drops_an_idle_pool_and_allows_recreation() {
        let slot = ConversionPool::new();
        let first = slot
            .acquire(2)
            .expect("a multi-worker pool should be built");
        let first_weak = Arc::downgrade(&first);
        drop(first);

        slot.release();
        assert!(first_weak.upgrade().is_none());

        let second = slot.acquire(2).expect("the pool should be recreated");
        assert!(Arc::strong_count(&second) >= 2);
    }

    #[test]
    fn release_preserves_a_pool_acquired_by_in_flight_work() {
        let slot = ConversionPool::new();
        let acquired = slot
            .acquire(2)
            .expect("a multi-worker pool should be built");
        let weak = Arc::downgrade(&acquired);

        slot.release();
        assert!(weak.upgrade().is_some());

        acquired.install(|| {});
        drop(acquired);
        assert!(weak.upgrade().is_none());
    }
}

//! Shared, dependency-free instrumentation for the memory regression examples.
//!
//! Heap counters record requested System allocator bytes, excluding OS mappings.
//! RSS includes mappings and allocator retention. Run each scenario in a fresh
//! process; compare identical binaries built with the workspace release profile.

use std::{
    alloc::{GlobalAlloc, Layout, System},
    hint::black_box,
    sync::atomic::{AtomicU64, Ordering},
    time::Instant,
};

struct CountingAllocator;
static LIVE: AtomicU64 = AtomicU64::new(0);
static PEAK: AtomicU64 = AtomicU64::new(0);
static ALLOCATED: AtomicU64 = AtomicU64::new(0);
static ALLOCATIONS: AtomicU64 = AtomicU64::new(0);

fn allocated(size: usize) {
    let live = LIVE.fetch_add(size as u64, Ordering::Relaxed) + size as u64;
    PEAK.fetch_max(live, Ordering::Relaxed);
    ALLOCATED.fetch_add(size as u64, Ordering::Relaxed);
    ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
}

unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            allocated(layout.size());
        }
        pointer
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() {
            allocated(layout.size());
        }
        pointer
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        LIVE.fetch_sub(layout.size() as u64, Ordering::Relaxed);
        unsafe { System.dealloc(pointer, layout) };
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let result = unsafe { System.realloc(pointer, layout, size) };
        if !result.is_null() {
            LIVE.fetch_sub(layout.size() as u64, Ordering::Relaxed);
            allocated(size);
        }
        result
    }
}

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

#[cfg(target_os = "macos")]
fn rss() -> Option<u64> {
    // PROC_PIDTASKINFO starts with virtual_size and resident_size (both u64).
    #[link(name = "proc")]
    unsafe extern "C" {
        fn proc_pidinfo(pid: i32, flavor: i32, arg: u64, buffer: *mut u8, size: i32) -> i32;
    }
    let mut info = [0_u64; 32];
    let written = unsafe {
        proc_pidinfo(
            std::process::id() as i32,
            4,
            0,
            info.as_mut_ptr().cast(),
            std::mem::size_of_val(&info) as i32,
        )
    };
    (written >= 16).then_some(info[1])
}

#[cfg(target_os = "linux")]
fn rss() -> Option<u64> {
    unsafe extern "C" {
        fn sysconf(name: i32) -> i64;
    }
    let resident = std::fs::read_to_string("/proc/self/statm")
        .ok()?
        .split_whitespace()
        .nth(1)?
        .parse::<u64>()
        .ok()?;
    let page_size = unsafe { sysconf(30) };
    (page_size > 0).then_some(resident * page_size as u64)
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn rss() -> Option<u64> {
    None
}

pub fn phase(scenario: &str, label: &str, width: u32, height: u32, logical_bytes: usize) {
    let resident = rss().map_or_else(|| "null".to_owned(), |bytes| bytes.to_string());
    println!(
        "{{\"record\":\"phase\",\"scenario\":\"{scenario}\",\"phase\":\"{label}\",\"width\":{width},\"height\":{height},\"logical_bytes\":{logical_bytes},\"rss_bytes\":{resident},\"heap_live_bytes\":{},\"heap_peak_bytes\":{},\"heap_allocated_bytes\":{},\"heap_allocations\":{}}}",
        LIVE.load(Ordering::Relaxed),
        PEAK.load(Ordering::Relaxed),
        ALLOCATED.load(Ordering::Relaxed),
        ALLOCATIONS.load(Ordering::Relaxed),
    );
}

#[allow(
    clippy::assertions_on_constants,
    reason = "compile examples for linting in debug, but require release when running"
)]
pub fn arguments(default: &str) -> (String, usize) {
    assert!(
        !cfg!(debug_assertions),
        "memory benchmarks require --release"
    );
    let mut args = std::env::args().skip(1);
    let scenario = args.next().unwrap_or_else(|| default.to_owned());
    let repetitions = args.next().map_or(31, |arg| arg.parse().unwrap());
    assert!(repetitions > 0, "repetitions must be positive");
    (scenario, repetitions)
}

pub fn measure(
    scenario: &str,
    width: u32,
    height: u32,
    repetitions: usize,
    logical_bytes: usize,
    mut operation: impl FnMut() -> u64,
) {
    for _ in 0..3 {
        black_box(operation());
    }
    let mut samples = Vec::with_capacity(repetitions);
    PEAK.store(LIVE.load(Ordering::Relaxed), Ordering::Relaxed);
    ALLOCATED.store(0, Ordering::Relaxed);
    ALLOCATIONS.store(0, Ordering::Relaxed);
    for _ in 0..repetitions {
        let started = Instant::now();
        let checksum = black_box(operation());
        samples.push((started.elapsed().as_nanos(), checksum));
    }
    phase(scenario, "live", width, height, logical_bytes);
    for (iteration, (elapsed_ns, checksum)) in samples.into_iter().enumerate() {
        println!(
            "{{\"record\":\"sample\",\"scenario\":\"{scenario}\",\"width\":{width},\"height\":{height},\"iteration\":{iteration},\"elapsed_ns\":{elapsed_ns},\"checksum\":{checksum},\"logical_bytes\":{logical_bytes}}}"
        );
    }
}

/// Sample every 4096th byte plus both ends. Keep verification inexpensive and
/// identical across revisions; workloads also assert dimensions and sentinel pixels.
pub fn checksum(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .step_by(4096)
        .chain(bytes.last())
        .fold(bytes.len() as u64, |hash, byte| {
            hash.wrapping_mul(0x100_0000_01b3) ^ u64::from(*byte)
        })
}

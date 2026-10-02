# Pixel buffer storage

This Apache-2.0, header-only implementation supplies private storage to Snow Image,
Snow Draw Engine Qt, and the Snow Shot codec bridge. It has no third-party or
runtime library dependency. Include `cmake/SnowPixelBuffer.cmake` and use the
`snow_pixel_buffer` build-interface target; installed library APIs do not expose it.

Buffers below 1 MiB use the heap. Larger buffers own anonymous VM regions
(`mmap` on macOS/Unix, ordinary `VirtualAlloc` pages on Windows). Releasing the
last owner returns those pages immediately instead of leaving them in malloc
caches. The threshold avoids a mapping per small raster or render tile.
Allocation is zero-initialized and returns null on failure; large allocations
never fall back to a caching heap.

Prefer the movable `PixelBuffer` owner. When transferring ownership across an
existing C ABI, retain the original allocation size and pair `release()` with
`releasePixelBuffer(pointer, originalSize)` in the allocating module. Never pass
an interior pointer or a logical subview's size to that release function.

`snowCanvasAllocateImage` attaches this owner to a writable external `QImage`.
Qt shallow copies retain it; painting a unique image does not allocate new pixel
storage. Qt's own implicit detach, scale, and format-conversion operations can
still allocate through Qt's heap. The helper is used at raster creation sites,
not to copy already-created rasters into another allocation.

Rust capture frames implement the equivalent ownership in `frame_pages.rs`,
using Mach VM on macOS and VirtualAlloc on Windows. Active capture retains its
existing frame recycling, capacity headroom, and copy-on-write behavior.

Focused regression coverage: `snow-canvas-image-tests` verifies native unmapping,
sharing, cross-thread destruction, painting, alignment, and invalid dimensions;
`cargo test -p snow-capture --lib frame` verifies frame recycling, copy-on-write,
and final-owner VM release. Neither test relies on RSS timing or heap purging.

Build `snow-canvas-image-memory-benchmark` with the platform's **performance**
preset, then run `--qt` and `--pages` in separate fresh processes. It compares
60 allocate/fill/release cycles of capture-sized, selection-sized, and output
rasters, reporting memory and p50/p95 latency. This isolates allocator retention;
it does not measure the application's native window or capture-framework caches.

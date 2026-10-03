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

`PixelArray<T>` gives private numeric raster workspaces vector semantics while
releasing their original capacity through the same allocator. Public library
containers retain their existing types; private codec staging, animation,
filter, shadow, and reconstruction scratch use this storage policy.

`snowCanvasAllocateImage` attaches an owner to writable external `QImage` pixels.
Qt shallow copies retain it. Use `snowCanvasDetachImage` before changing shared
pixels or metadata, and the copy/crop, conversion, orientation, and scale helpers
when producing another raster. Unique managed images keep their storage;
read-only capture views become independently writable. Conversion work is
bounded to small strips, including very wide rows. Common orientations write
directly into the final allocation and preserve Qt's pixel results.

Qt's smooth scaler, indexed dithering, unusual transforms, and plugins that
replace a decoder destination still use a Qt temporary before the final large
raster is copied into managed storage. These helpers do not change resampling
or dithering to reduce allocations. `image_codec::readManagedImage` decodes
compatible Qt readers directly into managed pixels and retains allocation limits,
profiles, text, DPI, and high bit depths.

Rust's `snow-memory` crate provides `RasterBuffer` and sealed numeric
`RasterArray` owners with the same threshold. They support capture copy-on-write,
frame recycling, initialized capacity headroom, scrolling tiles and staging,
recording compositors/export/playback, and OCR input storage. Windows retains
capture's large-page attempt with ordinary VM-page fallback. Legacy public Vec
adapters are explicit boundaries; the application uses owned buffers internally.

Existing bounded pools retain storage while active and release it with their
owner. Codec/framework/model allocations, GPU textures, public EXR sample
vectors, compressed payloads, and native window caches remain controlled by
those components. Immediate page release applies to managed raster owners, not
the process's total RSS.

Focused regressions exercise actual decoding, cancellation, conversion,
copy-on-write, orientation, rendering, workspaces, reconstruction, C ABI leases,
and recording reuse. They check exact pixels and native mapping ownership,
including final release after truncation and cross-thread destruction. They do
not depend on RSS timing or heap purging.

Build `snow-canvas-image-memory-benchmark` with the platform's **performance**
preset, then run `--qt` and `--pages` in separate fresh processes. It compares
60 allocate/fill/release cycles of capture-sized, selection-sized, and output
rasters, reporting memory and p50/p95 latency. This isolates allocator retention;
it does not measure the application's native window or capture-framework caches.

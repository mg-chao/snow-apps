# Memory Optimization Review — commit `70bfa5b37e628c50b0b803ab168309c8aa5481d4` (`perf memory`)

This document inventories every memory-optimization direction introduced by commit
`70bfa5b37e628c50b0b803ab168309c8aa5481d4` (branch `perf/20260827_memory`, 61 files,
+4508/−1193). It is structured for **item-by-item review**: each section describes the
optimization goal, the mechanism, the touched files, key constants/ownership rules, the
covering tests, and a review checklist.

Companion measurement/verification infrastructure added by the same commit is described in
Section 8. A consolidated risk list is in Section 9.

---

## 1. Large pixel buffers bypass the CRT/Qt heap — `VirtualAlloc` storage returned to the OS on drop

**Goal.** Large raster buffers allocated in the process heap (Rust `Vec`, `QImage`'s internal
`malloc`) are effectively *retained* after free on Windows: the NT heap decommits lazily and
the low-fragmentation/segment heap keeps regions for reuse, so private working set stays high
after a screenshot closes. Allocating these buffers directly from the kernel
(`VirtualAlloc`/`VirtualFree`) makes the release immediate and exact.

### 1.1 Rust `PixelStorage` in the stitch engine

* Files: `snow-crates/crates/snow-stitch-images/src/frame.rs`,
  `snow-crates/crates/snow-stitch-images/src/tiled_canvas.rs`.
* New `PixelStorage` enum: `Heap(Vec<u8>)` or (Windows-only) `Virtual(VirtualPixels)`.
  * `VirtualPixels` wraps `VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT,
    PAGE_READWRITE)`; `Drop` calls `VirtualFree(ptr, 0, MEM_RELEASE)`; allocation failure goes
    through `handle_alloc_error`. `unsafe impl Send/Sync`.
  * **Threshold:** `RECLAIMABLE_PIXEL_ALLOCATION_THRESHOLD = 1 MiB`. `PixelStorage::zeroed`
    and `copied_from_slice` choose `Virtual` only when `len >= 1 MiB` (Windows only;
    non-Windows always uses `Vec`).
  * `Frame::pixels` changed `Vec<u8>` → `PixelStorage`. `Frame::from_strided` now
    pre-sizes one `PixelStorage::zeroed(packed_len)` and copies rows in place (previously
    `Vec::with_capacity` + `extend_from_slice`).
  * `CanvasTile::pixels` in `tiled_canvas.rs` likewise became `PixelStorage`; horizontal
    slicing builds the result with `PixelStorage::zeroed` + in-place row copies.
  * `Frame::encode` no longer clones pixels — it copies out via `to_vec()` only where the
    `image` crate needs an owned buffer; `into_pixels()` converts storage back to `Vec`.
  * `VirtualPixels::resize` beyond capacity reallocates + copies (no in-place growth);
    `truncate` only shrinks the logical length; `Clone` deep-copies into a new virtual
    allocation.
* Tests: `large_strided_frames_use_release_on_drop_storage` (frame ≥ 1 MiB uses reclaimable
  storage, clone equality, `into_pixels`).

**Review checklist**
- [ ] 1 MiB threshold appropriate for the tile (256-row) and frame sizes actually produced?
- [ ] Every path that produced a `Frame`/`CanvasTile` ≥ 1 MiB now benefits (decode, push,
      snapshot slices, PNG export path)?
- [ ] Double-buffering cost: `resize`/`clone`/`to_vec` temporarily hold two copies — any
      path that clones large frames repeatedly?
- [ ] `handle_alloc_error` aborts the process on VirtualAlloc failure — is that the desired
      policy for OOM during stitching?
- [ ] Alignment/zeroing semantics: VirtualAlloc is zero-filled; `zeroed(len, value)` fills
      non-zero `value` correctly (`resize` fills the delta) — verify fill coverage on grow.
- [ ] Non-Windows builds silently keep old behavior (heap retention) — accepted divergence?

### 1.2 Qt `allocateTransientImage` — `QImage` over OS-released storage

* Files: `snow_draw_engine_qt/include/snow_draw_engine_qt/snow_transient_image.h`,
  `snow_draw_engine_qt/src/core/snow_transient_image.cpp` (new),
  `snow_draw_engine_qt/cmake/SnowCanvasQtSources.cmake`,
  `snow_draw_engine_qt/CMakeLists.txt` (new test target).
* `allocateTransientImage(size, format)` returns a `QImage` whose backing memory is
  `VirtualAlloc`'d (Windows) or `new uchar[]` (elsewhere) and is released through QImage's
  cleanup-function hook (`VirtualFree(..., MEM_RELEASE)` / `delete[]`) when the **last
  shallow-copy owner** is destroyed. Qt implicit-sharing detach is preserved (verified by
  test). Only 32-bpp formats are supported; dimensions/overflow fully checked; stride is
  exactly `width * 4` (no padding).
* Call sites switched from plain `QImage(width, height, fmt)` constructors:
  * `snow_draw_engine_qt/src/rendering/snow_canvas_export.cpp` — export output image **and**
    the full-size scene `background` buffer in `renderToImage`.
  * `snow_shot/src/presentation/overlay/screenshotselectionshadowrenderer.cpp` —
    `ScreenshotResultCompositor::compose` output image and the rounded-corner `mask` buffer.
  * `snow_shot/src/presentation/services/screenshotsourceimagecomposer.cpp` — the
    `RGBA8888` selection source composition image.
* Tests: `snow_draw_engine_qt/tests/snow_transient_image_tests.cpp` (format/stride/size
  validation, QPainter writes, shallow-copy detach, and on Windows a `VirtualQuery`
  MEM_COMMIT check proving release exactly when the final shallow owner dies).

**Review checklist**
- [ ] All large, short-lived raster allocations in the export/compose paths are covered —
      any remaining multi-MB `QImage(...)` constructor on a hot path (e.g. scale/convert
      intermediates inside `renderToImage`, `QPixmap` caches)?
- [ ] Cleanup function runs on whichever thread destroys the last owner — confirm no
      cross-thread QImage handoff where the final owner thread differs from the allocating
      thread in a way that matters (VirtualFree is thread-agnostic; fine, but document).
- [ ] Failure mode when allocation fails: callers return null/empty result — check UX for
      huge exports (error surfaced, not silent).
- [ ] `snow-shot-screenshot-result-compositor-tests` now compiles
      `snow_transient_image.cpp` directly (CMake) — keep include dirs in sync.

### 1.3 OCR filled-image buffer

* File: `snow_shot/src/presentation/ocr/screenshotocrrecognitionservice.cpp`.
* The RGBA "filled image" returned by the OCR worker is read into a `VirtualAlloc` buffer
  and wrapped in a `QImage` with `releaseImageBuffer` cleanup (previously the buffer came
  from the Rust FFI's owned image). Same immediate-release semantics as 1.2.

---

## 2. OCR recognition moved out of process

**Goal.** The ONNX Runtime (+ optional DirectML) engine, models, and inference arenas —
the single largest memory consumer in the app — must never be resident in the main
`snow_shot` process. Previously each in-process worker thread kept a persistent
`SnowOcrEngine` (created once per worker, retained across jobs, and kept warm between
backend switches).

### 2.1 Worker executable and entry point

* `snow_shot/src/app/main.cpp`: `snow_shot --screenshot-ocr-worker` branches into
  `runScreenshotOcrWorker()` **before any `QCoreApplication` is constructed** — the child
  never initializes Qt GUI, only the Rust OCR stack (demand-paged code keeps the shared
  binary cheap).
* `snow_shot/src/presentation/ocr/screenshotocrworkerentry.{h,cpp}` (new): reads one
  request from stdin, creates an engine, recognizes, writes one response to stdout, exits.
  * Engine config in the child: threads = `max(1, physical_cores / 2)` (unchanged formula,
    now applied in the child).
  * stdin/stdout switched to binary mode on Windows; strict request validation.

### 2.2 Wire protocol

* `snow_shot/src/presentation/ocr/screenshotocrworkerprotocol.h` (new):
  `#pragma pack(1)` headers with static size asserts.
  * `RequestHeader` (32 B): magic `0x534f4352`, version 1, width/height/stride,
    `useDirectMl`, `rgbaLength`.
  * `ResponseHeader` (48 B): status, echoed dimensions, `rgbaLength`, `lineCount`,
    `errorLength`.
  * `LineHeader` (48 B): text length, confidence, 8 quad floats, RGBA foreground.
  * Limits: image ≤ **512 MiB**, lines ≤ **1,000,000**, total text ≤ **64 MiB**.

### 2.3 Client-side process wrapper

* `snow_shot/src/presentation/ocr/screenshotrecognitionworkerprocess.{h,cpp}` (new):
  `RecognitionWorkerProcess` — `CreateProcessW` with `CREATE_NO_WINDOW` + two 64 KiB
  anonymous pipes on Windows (non-Windows: `QProcess`); chunked (64 KiB) cancellable
  `readExact`/`writeExact` (25 ms poll granularity); `closeInput` (EOF signal);
  `waitForFinished`; `exitedSuccessfullyWithoutOutput` (exit code 0 **and** pipe fully
  drained); `terminate` (TerminateProcess + wait) invoked from the destructor.
* `snow_shot/src/presentation/ocr/screenshotocrrecognitionservice.cpp` (rewritten):
  * `runRecognition` no longer takes a persistent engine; per job it spawns the worker
    (executable = `SNOW_SHOT_OCR_WORKER_EXECUTABLE` env var or
    `QCoreApplication::applicationFilePath()`), streams the RGBA8888 image, parses the
    validated response, and requires a clean child exit. Cancellation returns an empty
    result instead of an error.
  * Response parsing validates every header field (dimension echo, stride/length
    consistency, finiteness of floats, cumulative text bound) before allocating.
  * Worker slots no longer own engines; the `engine.reset()` lifecycle code is gone.
  * New service API:
    * `prewarmRuntime()` → `snow_ocr_runtime_initialize()` (new FFI export in
      `snow-crates/crates/snow-ocr-c`, calling `ort::init().commit()`): initializes the
      ONNX Runtime environment once, idempotent, creates **no** engines/workers/results
      (asserted by test).
    * `releaseRetainedIdleResources(completion)` → completion fires once all OCR workers
      have drained (no live/active workers, no queued jobs, no pending requests).
* `snow_shot/include/snow_shot/presentation/screenshotocrrecognitionservice.h` updated.

**Effect.** All OCR/DirectML memory is reclaimed by process exit; the main process holds
only the streamed-in result buffers (themselves VirtualAlloc-backed, §1.3). FFI resource
counters (`engines`, `results`, `owned_images`) in the main process are now expected to be
**0** after a job (tests assert this; previously `owned_images == kRequestCount` while
results were alive in-process).

**Review checklist**
- [ ] Per-job cost: each recognition now pays process spawn + model load. Quantify latency
      regression vs. the memory win; consider a short-lived worker reuse window (e.g. N
      seconds) if OCR is triggered repeatedly.
- [ ] `prewarmRuntime()` loads onnxruntime into the **main** process at startup
      (`ScreenshotController::prewarmResources`). With recognition fully out of process,
      confirm the intended benefit (warm OS file cache for the child's DLL/model load,
      early failure detection) outweighs the main-process baseline cost of a mapped
      onnxruntime.
- [ ] Worker executable resolution: `applicationFilePath()` re-exec — safe under
      MSIX/install layouts, symlinks, and when the running binary is a test harness?
- [ ] Pipe deadlocks: request image (≤ 512 MiB) is written before the response is read;
      the child reads the full request before writing (worker reads then writes) — no
      overlap deadlock, but confirm the 64 KiB pipe + chunked write with 25 ms polls
      cannot stall against a slow child (60 s timeouts exist only in the clipboard path;
      the OCR path has **no overall timeout** — cancellation is the only exit).
- [ ] `terminate()` on cancellation kills the child mid-write; verify no orphaned child if
      the parent thread is destroyed first (destructor terminates — OK) and that
      `TerminateProcess` during DirectML inference cannot leak GPU/driver state that
      outlives the child.
- [ ] Protocol hardening: version/magic/reserved-field checks on both sides; error text
      truncated at 64 MiB; child exit codes 2 (bad request header), 3 (I/O failure).
- [ ] Non-Windows path uses `QProcess` from a worker thread — `waitForReadyRead(25)` loop
      is CPU-light; confirm no busy-spin when the child is computing.
- [ ] `exitedSuccessfullyWithoutOutput` treats leftover bytes as failure — good; verify
      stderr passthrough on Windows is not needed for diagnosability (currently discarded;
      non-Windows surfaces it via `errorString()`).

---

## 3. Clipboard publishing moved to a dedicated helper process

**Goal.** Decouple clipboard ownership (retry loops, hidden owner window, HGLOBAL
lifetime) from the long-lived GUI process, and let the big DIB payload be published and
forgotten without retaining clipboard-side state in the app.

* New executable `snow-shot-clipboard-worker`
  (`snow_shot/src/presentation/services/screenshotclipboardworker.cpp` +
  `screenshotclipboardworkerprotocol.h`, built by `snow_shot/CMakeLists.txt` as a WIN32
  GUI-subsystem app, AUTOMOC off):
  * Reads `RequestHeader` (24 B: magic `0x53434252`, version, `nativeFormat`
    CF_DIB/CF_DIBV5, `payloadBytes` ≤ **512 MiB**) + payload from stdin into its own
    `GlobalAlloc(GMEM_MOVEABLE)` block.
  * `validDibPayload` enforces a strict 32-bpp, exact-size, BI_RGB (CF_DIB, top-down) /
    BI_BITFIELDS + RGBA masks (CF_DIBV5, bottom-up) contract before publishing.
  * Creates a message-only `HWND`, opens the clipboard with **5 attempts**
    (10/25/60/100 ms backoff), `EmptyClipboard` + `SetClipboardData`, destroys the window,
    writes a `ResponseHeader` (status, native error, attempts), exits.
* Client side (`snow_shot/src/presentation/services/screenshotclipboardservice.cpp`):
  * The old static `clipboardOwnerWindow()` HWND and in-process
    Open/Empty/Set/Close path are removed.
  * `publishClipboardPayload` now streams the HGLOBAL contents to the worker over stdin
    (64 KiB chunks, **60 s** overall timeout, cancellation-aware), then validates the
    fixed-size response and maps worker status → existing
    `ScreenshotClipboardCommitFailure` values.
  * `ClipboardCommitOperation` gained `backgroundAttempt`: on Windows the attempt runs on
    a detached `std::thread` and re-enters the object via queued invocation (receiver
    destruction while in flight is handled; attempt counters aggregated from worker
    response).
  * Worker discovery: `QCoreApplication::applicationDirPath() +
    "snow-shot-clipboard-worker.exe"`; CMake stages the worker next to **every** target
    that compiles `screenshotclipboardservice.cpp` (tests/benchmarks included) and
    installs it.
* Perf counters extended: `clipboard.failure.worker_missing`, `worker_io`, `worker_protocol`,
  `worker_publish`, `clipboard.cancelled`, `clipboard.worker_bytes`.

**Review checklist**
- [ ] Main-process HGLOBAL lifetime: `publishClipboardPayload` no longer transfers
      ownership (`*nativeHandle = nullptr` on success is gone). Verify the
      `ScreenshotClipboardPayload` destructor frees the main-process allocation after a
      successful publish, i.e. the big DIB does not linger until the next payload replaces
      it (this is now the main retention risk of this change).
- [ ] Transient double copy (main HGLOBAL + worker GlobalAlloc) during publish — peak
      memory is briefly ~2× DIB size; acceptable for 512 MiB cap?
- [ ] Detached `std::thread` inside a QObject-based operation: process exit while a
      publish is in flight — the detached thread is not joined; check interaction with
      `ScreenshotAsyncActivityLease` and Qt shutdown ordering.
- [ ] Worker `GlobalLock` write + validation happen before publish — if validation fails
      the global is freed and `InvalidPayload` returned; confirm no partial-clipboard
      state (EmptyClipboard not yet called at that point — OK).
- [ ] `wWinMain` + `ReadFile` on stdin with no cancellation: a crashed parent leaves the
      worker blocked on stdin until pipe close kills it — confirm pipe handle inheritance
      (parent ends are `SetHandleInformation(..., HANDLE_FLAG_INHERIT, 0)`-protected) and
      that the worker exits when stdin closes.
- [ ] Failure mapping parity: Busy/ClearFailed/PublishFailed semantics and retry counters
      match the old in-process behavior (tests updated in
      `screenshot_clipboard_content_tests.cpp` / commit-attempt plumbing).
- [ ] Security/robustness: worker trusts the parent pipe only; payload cap 512 MiB; no
      path for the worker to outlive a successful publish and hold the clipboard open.

---

## 4. Bounded and fully released worker/thread pools

**Goal.** Thread stacks (default 1–8 MiB reserved/committed per thread) and pool growth
are a major hidden cost; pools must be bounded, sized to physical cores, and destroyed —
not just idle — when screenshot sessions end.

### 4.1 Bounded per-session rayon executor in the stitch FFI

* Files: `snow-crates/crates/snow-stitch-images-c/src/lib.rs`,
  `snow-crates/crates/snow-stitch-images-c/Cargo.toml` (+`rayon`, +`num_cpus`;
  the `snow-crates/Cargo.lock` and `snow_rust_ffi/Cargo.lock` dependency-lock
  updates ride along).
* Every `StitchSession` now owns an `Arc<rayon::ThreadPool>` built by
  `build_stitch_executor()`: thread count = `min(available_parallelism,
  num_cpus::get_physical(), MAX_STITCH_WORKERS)` with `MAX_STITCH_WORKERS = 4`, threads
  named `snow-stitch-{i}`.
* All compute entry points (`push`, `copy_rows`, `snapshot_axis`, `materialize*`,
  `render_scaled*`, snapshot ops, PNG export) run inside `executor.install(...)`.
* The executor is `Arc`-shared with `StitchImageSnapshot` (and slices) and leased by
  `PngExportTask`; it is destroyed when the **last** of session/snapshots/export tasks
  drops (`PngExportTask::wait` drops its lease after joining).
* `snow_stitch_snapshot_copy_rows` FFI now calls the snapshot-level
  `copy_rows_strided` (which installs the executor) instead of reaching into
  `snapshot.canvas` directly.
* Tests: executor is bounded and freed with an idle session; snapshots keep it alive until
  the last slice drops; completed PNG export releases its lease; repeated sessions do not
  accumulate owners.

**Review checklist**
- [ ] Previously the code (canvas materialize etc.) ran without an explicit pool —
      confirm nothing previously relied on rayon's **global** pool and its thread reuse
      across sessions (per-session pools now pay thread spawn cost per session).
- [ ] `MAX_STITCH_WORKERS = 4` vs. physical cores on high-end machines — throughput
      regression risk for large scrolls?
- [ ] Leak audit: any FFI handle path (forgotten `snow_stitch_*_destroy`) now leaks a
      whole thread pool — check the C-side owners in `screenshotscrollingcapturecontroller`.

### 4.2 Export-service worker thread lifecycle (start on demand, stop when idle)

* Files: `snow_shot/src/presentation/services/screenshotexportservice.cpp`,
  `.h`.
* The dedicated export `QThread` is created but **not started** in the constructor; the
  `ScreenshotExportWorker` object is created and the thread started lazily by
  `ensureWorkerRunning()` on the first accepted request.
* New `releaseRetainedIdleResources(completion)`: when a worker exists, it is invoked on
  the worker thread to `m_runtime.reset()` (its `SnowCanvasRuntime`) and clear the render
  caches (§5); after pending requests drain, the thread is `quit()` + `wait()`ed and the
  worker deleted. The next request transparently recreates it.
* Requests no longer fail when the thread is not running (they start it).

**Review checklist**
- [ ] `releaseRetainedIdleResources` races a concurrently arriving request: the release
      completion re-checks `hasPendingRequests()` and reports `false` — confirm a request
      scheduled between the worker-thread lambda and the completion-context lambda cannot
      be dropped (request path re-checks `m_worker == nullptr` via `ensureWorkerRunning`).
- [ ] Thread churn: quit/start per screenshot session — measure spawn cost vs. retained
      stack memory; consider a reuse window if regression shows.

### 4.3 Qt private GUI image thread pool capped during capture (Windows)

* File: `snow_shot/src/presentation/services/screenshotcontroller.cpp`.
* On `beginCapture`, `limitQtGuiImageThreadPoolToRetainedWorkers()` caps
  `QGuiApplicationPrivate::qtGuiThreadPool()->setMaxThreadCount()` to the **currently
  resident** worker count (via `QThreadPoolPrivate::allThreads.size()`), so full-screen
  `QImage` conversion bursts cannot grow the pool beyond the prewarmed baseline.
* After cold hibernation completes, `restoreQtGuiImageThreadPoolMaximum()` puts the
  configured maximum back (and drains `DeferredDelete` before publishing
  `captureReleased`).

**Review checklist**
- [ ] Uses Qt private headers (`qguiapplication_p.h`, `qthreadpool_p.h`) — pin against Qt
      version upgrades; `allThreads` accounting while a task is starting may undercount.
- [ ] Capping at capture start means conversions during capture are serialized to the
      prewarmed worker set — check capture latency on many-monitor setups.
- [ ] Restore path is inside the hibernation completion; if hibernation is cancelled
      (generation mismatch), the restore must still happen — verify every early-return
      path eventually restores (currently the restore only runs in the final queued turn
      that also publishes `captureReleased`; a superseded generation skips it and the next
      beginCapture re-caps — confirm no path leaves the pool permanently capped).

### 4.4 Capture workflow "cold hibernation" (IdlePrepared → IdleCold)

* Files: `snow_shot/src/presentation/capture/screenshotcaptureworkflow.cpp`,
  `.h` (comment), tests.
* The `IdleResourcePolicy::Hibernate` branch previously kept a prepared core
  (`clearDisplays` + `resetForNewCapture` + `hibernateDisplayPool` → `IdlePrepared`,
  worker retained). Now it performs a **cold** teardown while preserving the workflow
  object graph: `destroySelectorService()`, `destroyDisplayPool()`, then after
  `releaseIdleResourcesAsync` completes (guarded against supersession),
  `shutdownCaptureWorker()` and state → `IdleCold`. A subsequent capture re-enters through
  `ensureCaptureWorker` (test asserts `ensureCaptureWorkerCalls == 1` after hibernation).
* Tests renamed/updated: `retainedCancellationColdHibernatesWithoutRebuildingTheObjectCore`
  (`hibernateDisplayPoolCalls == 0`, `shutdownCaptureWorkerCalls == 1`).

**Review checklist**
- [ ] Trade-off: fast-restart window is gone (every idle release now pays worker + display
      recreation on next capture). Confirm the memory win justifies it (benchmark
      `closed` stage should show it).
- [ ] `finishColdHibernation` aborts (completes `false`) if the session was superseded —
      verify the controller's release barrier tolerates the aborted path.

---

## 5. Idle-time release orchestration in the controller (fine-grained hibernation instead of Impl destruction)

**Goal.** Previously "idle release" destroyed the whole lazy `ScreenshotController::Impl`
(`m_impl.reset()`), which forced rebuilding the workflow graph on the next capture. The
new model keeps the lightweight object core alive and releases the heavy leaves
explicitly.

* File: `snow_shot/src/presentation/services/screenshotcontroller.cpp` (+ headers).
* `Impl::releaseRetainedIdleResources` now builds a `ReleaseBarrier` aggregating
  asynchronous completions from:
  1. `m_scrollingCaptureController->releaseIdleResources()` (new: `stop(false)` +
     `shutdownWorker()` — drains the stitch executor and frame tiles; see
     `screenshotscrollingcapturecontroller.{h,cpp}`),
  2. `snow_capture_release_conversion_pool()` (pre-existing FFI from
     `snow-capture-c`; now also called during idle hibernation, not just teardown),
  3. `ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread()` (thread-local
     selection-sized shadow rasters),
  4. `m_tableRecognition->releaseRetainedIdleResources()` (synchronous bool),
  5. `m_exportService` (§4.2),
  6. `m_ocrRecognition` (§2.3),
  7. `m_qrRecognition` (`screenshotqrrecognitionservice.{h,cpp}` — idle-notification
     only, same pattern as OCR),
  8. `m_captureWorkflow->releaseRetainedIdleResources` chaining
     `SnowCanvasRuntime::releaseRetainedRenderResourcesForCurrentThread()` on the GUI
     thread — a new static that resets the **watermark render cache** and **hatch texture
     cache** (both thread-local, in `snow_canvas_renderer` / `snow_canvas_fill_render`)
     and clears the canvas **tile cache** (see §6).
* The final hibernation turn: drains `DeferredDelete`, restores the Qt GUI image pool
  maximum (§4.3), bumps `m_idleReleaseGeneration` (invalidates duplicate queued timers),
  and publishes the `captureReleased` lifecycle milestone. `m_impl.reset()` is **gone** —
  the Impl (models, dependencies, service graph) survives.
* `SnowShotApiClient` (`snow_shot/src/network/snowshotapiclient.cpp`, `.h`) gained
  `hasPendingRequests()` and `releaseRetainedIdleResources()` which deletes the lazily
  created `QNetworkAccessManager` child when idle (recreated on demand by
  `networkAccessManager()`); table-recognition release reaches it through
  `m_tableRecognition`.
* Header comment updated (`screenshotcontroller.h`): "hibernate captured rasters, native
  surfaces, render caches, and session workers" instead of tearing down the
  implementation.

**Review checklist**
- [ ] Coverage parity vs. the old `m_impl.reset()`: everything the destructor released
      must now be covered by the fine-grained list (compare `~Impl`'s reset sequence with
      the barrier contents; the destructor additionally resets workflow/runtime/
      presentation services — those are the deliberately retained "lightweight core";
      verify nothing heavy hides inside them, e.g. `SnowCanvasRuntime` state owned by
      `m_captureRuntime`).
- [ ] Barrier semantics: a subsystem whose `schedule` returns false calls
      `finish(false)` — the overall completion still fires with `released=false`; the
      controller ignores the bool (`Q_UNUSED(released)`) — confirm that's intended.
- [ ] `SnowCanvasRuntime::releaseRetainedRenderResourcesForCurrentThread` clears the tile
      cache globally (mutex-protected) but the watermark/hatch caches only for the
      **calling** thread — other threads that ever rendered (export worker is drained
      first, scrolling worker drained) — confirm no other long-lived thread holds those
      caches.
- [ ] `SnowShotApiClient::releaseRetainedIdleResources` deletes the NAM with `delete`
      while replies could still be parented to it — guarded by `hasPendingRequests()`;
      check QNetworkReply lifetime edge cases at the moment of deletion.
- [ ] 750 ms grace period and generation guards preserved in the restructured
      `scheduleIdleImplementationRelease` (nesting changed: single DeferredDelete drain →
      grace → barrier → final drain + restore).

---

## 6. Cache containers actually release memory on clear

* File: `snow_draw_engine_qt/src/rendering/snow_canvas_tile_cache.cpp`.
* `snow_canvas_tile_cache::clear()` now swaps every container (`tiles`, `masks`,
  `namespaces`, both `lru` lists, `maskLru`) with freshly constructed empties instead of
  `.clear()`, so the freed nodes/buckets are returned to the allocator/OS instead of the
  containers retaining capacity. Byte counters reset as before.

**Review checklist**
- [ ] Swap-with-empty releases `unordered_map` bucket arrays — confirm the allocator
      actually decommits (heap-dependent; the segment heap from §7 helps).
- [ ] `clear()` is invoked both from `releaseRetainedRenderResourcesForCurrentThread`
      and wherever it was called before — no behavior change beyond capacity release.

---

## 7. Windows heap policy: Segment Heap via embedded manifest

* Files: `snow_shot/resources/windows-app.manifest` (new),
  `snow_shot/resources/windows-app-resource.rc.in` (embeds it with
  `CREATEPROCESS_MANIFEST_RESOURCE_ID RT_MANIFEST`), `snow_shot/CMakeLists.txt`
  (configures/copies the manifest into the generated resource).
* The manifest declares for the main executable (and thus the OCR worker, which re-execs
  the same binary):
  * `<heapType>SegmentHeap</heapType>` (Windows 10 2004+): the modern segment heap with
    better decommit/trim behavior for mixed-size workloads — complements §1 (fewer large
    blocks in the heap at all) and §6 (decommit of swapped-out containers).
  * `supportedOS` Win10/11 GUID, `longPathAware`, `asInvoker`.

**Review checklist**
- [ ] Segment heap applies to all processes using the exe (main + OCR worker; the
      clipboard worker has **no** manifest — decide whether it needs one; its footprint is
      one DIB, so probably not).
- [ ] Behavior deltas to watch: `HeapEnableTerminationOnCorruption` defaults, LFH
      replacement, and third-party DLLs doing `HeapCreate`/walks.
- [ ] Confirm the RC embedding doesn't clobber a previously default-generated manifest
      (Qt/MinGW or MSVC default manifests; `snow_shot` previously had none in-repo).

---

## 8. Decode-path hardening and prewarm shaping (memory-adjacent)

### 8.1 Encoded-image signature validation before decode

* File: `snow_shot/src/presentation/services/screenshotclipboardcontent.cpp` (+ tests).
* `hasDeclaredImageSignature` checks magic bytes (PNG/JPEG/WebP/JXL codestream+container/
  AVIF ftyp-brand scan) before handing clipboard or on-disk bytes to the decoder when the
  declared format disagrees — prevents decoders from allocating large speculative buffers
  on mismatched/adversarial data and rejects stale files whose size/mtime no longer match.
* Review: signature coverage matches `kFileImageFormats`/`kEncodedImageFormats` (BMP/GIF
  not covered — they are not in the encoded set; TIFF?), AVIF brand scan bounded by box
  size.

### 8.2 Toolbar prewarm renders every tool palette

* File: `snow_shot/src/presentation/toolbar/screenshottoolbarwindow.cpp`.
* `prewarmForScreen` now iterates all 14 tool IDs, activating each palette entry and
  rendering a warm surface per tool (signals blocked), then `resetForNewCapture()`. Moves
  lazy per-tool allocation/materialization out of the first capture; warm pixmaps are
  transient and released immediately.
* Review: prewarm CPU/time cost at startup; no pixmap retained after the loop.

### 8.3 OCR runtime prewarm

* `ScreenshotController::prewarmResources` additionally calls
  `m_ocrRecognition->prewarmRuntime()` (§2.3) and warns on failure. See the §2.3 review
  item about whether the main process should load onnxruntime at all now.

---

## 9. Measurement & verification infrastructure added by the commit

These are not optimizations themselves but define how the optimizations are validated;
reviewing them confirms the claimed wins are real.

| Artifact | Purpose | Notes |
|---|---|---|
| `scripts/profile-screenshot-memory-lifecycle.ps1` (new) | End-to-end harness: launches the app and/or memory-footprint benchmark, samples region/thread/heap state across cold → active → post-release stages with stabilization waits (default 60 s cold-start min, 120 s per-stage min, 60 s post-release) | Integrates cdb (`CaptureDebuggerState`), heap snapshots (`CaptureHeapSnapshots`), region snapshots |
| `scripts/inspect-process-memory-regions.ps1` | VM-region dump grouped by allocation base | Now supports `-SampleMinimumPrivateWorkingSetMiB` (skip tiny private groups when sampling) and `-OutputPath` (UTF-8 file) |
| `scripts/inspect-process-threads.ps1` (new) | Per-thread description (`GetThreadDescription`) + start address (`NtQueryInformationThread(ThreadQuerySetWin32StartAddress)`) | Identifies which pools/threads survive idle release |
| `scripts/resolve-linker-map-addresses.ps1` (new) | Symbolizes thread start addresses against the MSVC linker `.map` (relocation from preferred base `0x140000000`) | Pairs with the thread probe |
| `scripts/analyze-heap-snapshot.ps1` (new) | Parses heap-snapshot exports, aggregates by allocation stack, diffs two snapshot instances | For finding retained heap blocks |
| `snow_shot/tests/screenshot_memory_footprint_benchmark.cpp` (rewritten, ~1183 lines changed) | UI-automation-driven, real-app benchmark; scenarios `MainInterface`, `ScreenshotWindow`, `PinToScreen`, `ScreenRecording`, `RightClickMenu`; stages `cold_start` / active / closed; **private working set** measured via `PROCESS_MEMORY_COUNTERS_EX2.PrivateWorkingSetSize` with `QueryWorkingSet` (Shared==0) fallback; per-stage stability-window convergence (median of converged window, plus peak/min/max/range/convergence_ms); lifecycle trace validation incl. the `captureReleased` milestone | This is the acceptance gate for §4–§6 |

**Review checklist**
- [ ] Run `profile-screenshot-memory-lifecycle.ps1` before/after the commit and record
      cold/closed private-working-set deltas per scenario; the `closed` stage is where
      §4/§5 should show improvement and §1/§6 should prevent the post-release tail.
- [ ] Confirm thread probe shows no leftover `snow-stitch-*`, export, OCR-worker, or
      expanded GUI-pool threads after `captureReleased`.

### Test coverage added (anchors for review)

* `snow-transient-image-tests` (§1.2), frame storage test (§1.1), stitch executor tests
  (§4.1).
* OCR service tests (`screenshot_ocr_recognition_service_tests.cpp`): isolated engine
  through the child process (CPU + DirectML), FFI resource counts zero in the main
  process, prewarm idempotence (`runtimePrewarmDoesNotCreateRecognitionResources`),
  idle release waits for cancelled recognition, worker retention while queued tasks
  remain; tests run the worker via `SNOW_SHOT_OCR_WORKER_EXECUTABLE=$<TARGET_FILE:snow_shot>`.
* Workflow tests: cold hibernation assertions (§4.4). Export service tests:
  `workerRuntimeHibernationDrainsAndReactivates` (§4.2). API client tests:
  idle NAM release. Clipboard content tests: signature validation.
* QR service tests (`screenshot_qr_recognition_service_tests.cpp`):
  `destroyingReceiverCancelsQueuedCompletion` extended — the
  `releaseRetainedIdleResources` completion is asserted to fire only after the
  cancelled worker thread has been destroyed (§5, item 7).
* Export coordinator tests (`screenshot_export_coordinator_tests.cpp`):
  `clipboardCommitRetriesTransientContention` extended — after the
  worker-process publish, the test reopens the clipboard and re-validates the
  CF_DIBV5 metadata (header size, dimensions/orientation, 32-bpp
  BI_BITFIELDS, alpha mask), proving the published payload survives the
  publisher process exiting (§3).
* CMake: canvas-renderer tests now link `onnxruntime` + FFmpeg and the offscreen
  platform plugin (side effect of the reorganized targets — verify this is required and
  not accidental link pollution).

---

## 10. Consolidated risk register (highest-leverage items first)

1. **Main-process HGLOBAL retention after clipboard publish** (§3) — ownership transfer
   semantics changed; verify the payload frees promptly after a successful worker publish.
2. **Fine-grained release coverage vs. old whole-Impl teardown** (§5) — any heavy
   subsystem missed by the barrier now leaks until process exit.
3. **OCR per-job spawn + model load latency** (§2) — measure; consider bounded worker
   reuse.
4. **Qt GUI thread pool restore on every early-exit path** (§4.3) — a permanently capped
   pool would serialize conversions app-wide.
5. **`prewarmRuntime` loading onnxruntime into the main process** (§2.3/§8.3) — confirm
   intended; contradicts the "keep ORT out of the main process" goal if not.
6. **No overall timeout on OCR worker I/O** (§2.3) — cancellation-only; a hung child
   pins a service worker thread forever.
7. **Private Qt API usage** (§4.3) and **detached publish thread** (§3) — fragility on Qt
   upgrades / shutdown ordering.
8. **Non-Windows parity** — VirtualAlloc paths fall back to heap (§1), Segment Heap is
   Windows-only (§7), clipboard worker is Windows-only (service keeps QClipboard path
   elsewhere).

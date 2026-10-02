# Snow Canvas filter benchmark

`snow-canvas-filter-benchmark` measures both the CPU image-filter kernels and the complete
Qt scene-rendering path. The kernel suite separates effect and opacity-blending costs. The
renderer suite includes sparse planning, spatial culling, working-surface allocation, effect
batching, mask composition, filtering, retained filter-source tile behavior, and final composition.

`snow-canvas-pen-filter-pipeline-benchmark` measures the interactive path from batched pointer
input through Rust composition, the incremental C ABI, the C++ display cache, mask generation,
and rendering. It also reports point reduction and the geometry points transported per patch:

```powershell
build-perf/Release/snow-canvas-pen-filter-pipeline-benchmark.exe `
  --samples 32000 --iterations 120 --trace noisy
```

Configure and build an optimized executable from a Visual Studio x64 developer shell:

```powershell
cmake -S snow_draw_engine_qt -B snow_draw_engine_qt/build-perf `
  -DSNOW_DRAW_ENGINE_QT_BUILD_DEMO=OFF `
  -DSNOW_DRAW_ENGINE_QT_BUILD_TESTS=ON `
  -DSNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS=ON
cmake --build snow_draw_engine_qt/build-perf --config Release `
  --target snow-canvas-filter-benchmark snow-canvas-pen-filter-pipeline-benchmark `
  snow-canvas-filter-render-tests
```

List scenarios, run a focused experiment, or save the complete result set:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
snow_draw_engine_qt/build-perf/Release/snow-canvas-filter-benchmark.exe --list
snow_draw_engine_qt/build-perf/Release/snow-canvas-filter-benchmark.exe `
  --scenario kernel_gaussian_high_1920x1080 --iterations 100
snow_draw_engine_qt/build-perf/Release/snow-canvas-filter-benchmark.exe `
  --suite all --csv snow_draw_engine_qt/build-perf/filter-baseline.csv
```

The default run uses 10 warmups and 100 measured iterations. Use `--warmup` and
`--iterations` to override those counts. Results include distribution
statistics and throughput; renderer rows also report exposed/working pixels, sparse components,
candidates and replayed items, original filters and effect dispatches, batching, memory traffic,
filter-source tile hits/misses/evictions, retained workspace bytes, Gaussian pass count, separate scene replay,
mask construction, downsample, reduced blur, reconstruction, and presentation timings, parallel
jobs, separate Gaussian downsample/reconstruction AVX2 execution counts, and the SIMD backend
that actually executed. CSV format version 7 names filter-source cache statistics explicitly and
removes the obsolete general scene diagnostics. It retains the version 6 queried/culled
pen chunks, rasterized pen tiles/pixels, pen-atlas hits/misses/evictions, tiles reused after geometry
patches, SIMD pen raster executions, and retained pen-atlas bytes. Pen rows no longer use the
whole-mask path-build or scan counters. The scenario matrix
includes DPR 1/1.25/2, cold/warm filter-source cache, overlay-only reuse, sparse distant dirty regions, 10K
mostly offscreen items, very-low/low/high Gaussian strengths, mixed filter layers, mixed strengths,
and forced execution backends.
CSV files use stable scenario names and
a `format_version` column so results can be retained as tuning baselines.

Run comparisons on the same machine, power profile, Qt version, compiler, build type, and
platform backend. Timings are diagnostic and intentionally have no machine-dependent pass/fail
threshold. Generated CSV files are build artifacts and should not be committed by default.

For release decisions, compare medians from the same machine and use a weighted geometric mean:
local and dirty Gaussian scenarios have weight 4, DPR2 scenarios have weight 2, and full-frame,
rotated, grouped, and mixed-strength scenarios have weight 1. The acceptance floor is 1.5x
overall, 1.3x for full 1080p Gaussian, and 1.5x for eight mixed strengths, with no stable
warm-cache or non-Gaussian regression above 5 percent.

Product Gaussian rendering deliberately uses an aggressive approximation: power-of-two reduction
keeps the reduced kernel compact, cached horizontal reconstruction avoids repeated interpolation,
and AVX2 performs opaque, constant-opacity, and Alpha8-mask reconstruction. The reference corpus
must retain per-fixture premultiplied SSIM 0.85, mean SSIM 0.90, valid premultiplied alpha, and
deterministic pixels within each execution backend.

## Emboss

Emboss scenarios cover 256x256, 1080p, and 4K kernels, single-threaded execution,
opaque and partial-alpha masks, full-frame rendering, local rendering on a 4K surface,
and a pen append at DPR 2. See [the emboss performance report](snow_canvas_emboss_benchmark.md)
for Release preset commands, before/after results, and exact-output validation.

## Pinned filter rendering

A host can override `SnowCanvasCustomRenderer::filterRenderReference()` with its full
canvas rectangle and original pixel density. Snow Shot enables this for materialized
pinned results, including Retina sources. The engine renders the background and ordered
scene once on that fixed grid, then presents the resulting image at every zoom and DPR.
Editor overlays, spotlight, and watermark remain in the live viewport path.

The widget owns one reference image, approximately `width * height * 4` bytes at the
reference resolution. Filter workspaces are temporary; pen masks reuse the widget's
bounded 16 MiB atlas across content edits. Standalone reference renders retain their own
bounded atlas when no host atlas is supplied. Erasers retain their pristine backdrop in
the shared 64 MiB source-cache budget; ordinary filter intermediates and zoom variants
are not retained. A fixed engine viewport preserves
source-pass boundaries and content that the live viewport culls. Document edits,
background `contentRevision()` changes, reference-grid changes, and explicit render-state
clearing replace the image. Hosts must increment their content revision for every
background appearance change.

Presentation matches ordinary pinned images: source pixels stay exact at 1:1 in device
pixels, and both reduction and enlargement use linear sampling without rerunning filters.

`snow-canvas-reference-scene-tests` checks rectangle and pen effects against the 100%
render, mixed source passes, transparent partial paints, DPR changes, panning, background
replacement, undo/redo, and runtime replacement. Warm presentation diagnostics require
zero filter dispatches, mask rasterization, execution-plan builds, and source-tile work.
`snow-shot-pinned-filter-reference-tests` checks the application integration and appearance
invalidation.

`snow-canvas-reference-scene-benchmark` compares the existing viewport path and reference
path while alternating a 1024x640 Gaussian/pen-mosaic scene between 70% and 130%. It reports
median and p95 for resizing, camera synchronization, and painting after four warmups and
40 measured frames. The reference path must dispatch zero filter effects during scaling.
Run only this benchmark from a performance preset:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-canvas-reference-scene-benchmark
QT_QPA_PLATFORM=offscreen build/snow-shot-macos-arm64-performance/snow_draw_engine_qt/snow-canvas-reference-scene-benchmark
```

On Windows, build the same target with `build-windows-msvc-performance` and run its
Release executable with `QT_QPA_PLATFORM=offscreen`.

## Background erasers

`snow-canvas-eraser-benchmark` measures Rectangle Eraser and Brush Eraser restoration,
immutable mask reuse, an eight-point append to a long brush stroke, and a local dirty
paint on a physical 4K canvas. See [the background eraser report](snow_canvas_eraser_benchmark.md)
for Release commands, cache limits, controlled comparison provenance, and results.

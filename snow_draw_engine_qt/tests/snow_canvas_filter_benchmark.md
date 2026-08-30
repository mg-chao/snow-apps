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

# Snow Canvas spotlight benchmark

`snow-canvas-spotlight-benchmark` measures the dedicated spotlight decoration renderer. CSV format
version 2 records p50/p95/p99 timing, processed and culled cutouts, early exits, fast paths, output
checksums, and environment metadata.

## Build and run

```powershell
cmake -S snow_draw_engine_qt -B snow_draw_engine_qt/build-perf `
  -DSNOW_DRAW_ENGINE_QT_BUILD_DEMO=OFF `
  -DSNOW_DRAW_ENGINE_QT_BUILD_TESTS=ON `
  -DSNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS=ON
cmake --build snow_draw_engine_qt/build-perf --config Release `
  --target snow-canvas-spotlight-benchmark

$env:QT_QPA_PLATFORM = 'offscreen'
snow_draw_engine_qt/build-perf/Release/snow-canvas-spotlight-benchmark.exe --list
snow_draw_engine_qt/build-perf/Release/snow-canvas-spotlight-benchmark.exe `
  --warmup 10 --iterations 300 `
  --csv snow_draw_engine_qt/build-perf/spotlight-results.csv
```

The stable scenario matrix covers 1920x1080 and 3840x2160, DPR 1, 1.25, and 2, and 1, 16, and
128 rotated cutouts. Additional scenarios cover fragmented and bounded exposure, opacity and color
preview bursts, zero visible cutouts, fractional geometry, small geometry changes, camera changes,
and render-area changes.

`legacy_reference_*` scenarios execute the repeated `QPainterPath::united()` algorithm inside the
benchmark only, providing same-build reference measurements without changing production behavior.

Functional or diagnostics mismatches return a nonzero exit code; timing values never do.

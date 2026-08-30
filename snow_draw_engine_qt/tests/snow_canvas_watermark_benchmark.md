# Snow Canvas watermark benchmark

`snow-canvas-watermark-benchmark` measures watermark rendering in isolation and through
the engine workflows used by Snow Shot. It is intended for same-machine performance tuning,
not for comparing different computers. The executable writes benchmark CSV format version 4.

## Build

Configure and build an optimized executable from a Visual Studio x64 developer shell:

```powershell
cmake -S snow_draw_engine_qt -B snow_draw_engine_qt/build-perf `
  -DSNOW_DRAW_ENGINE_QT_BUILD_DEMO=OFF `
  -DSNOW_DRAW_ENGINE_QT_BUILD_TESTS=ON `
  -DSNOW_DRAW_ENGINE_QT_BUILD_BENCHMARKS=ON
cmake --build snow_draw_engine_qt/build-perf --config Release `
  --target snow-canvas-watermark-benchmark snow-canvas-text-draft-tests
```

The benchmark requires a functioning Qt offscreen platform plugin and a system TrueType font.
On Windows it registers `C:/Windows/Fonts/segoeui.ttf` and reports the resolved family in every
CSV row.

## Run

Set the offscreen platform, list the stable scenario identifiers, run a focused experiment, or
write the complete result set:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
snow_draw_engine_qt/build-perf/Release/snow-canvas-watermark-benchmark.exe --list
snow_draw_engine_qt/build-perf/Release/snow-canvas-watermark-benchmark.exe `
  --scenario renderer_recolor_1920x1080 --iterations 300
snow_draw_engine_qt/build-perf/Release/snow-canvas-watermark-benchmark.exe `
  --suite all --csv snow_draw_engine_qt/build-perf/watermark-baseline.csv

python snow_draw_engine_qt/scripts/compare_watermark_benchmarks.py `
  snow_draw_engine_qt/build-perf/watermark-baseline.csv `
  snow_draw_engine_qt/build-perf/watermark-candidate.csv

python snow_draw_engine_qt/scripts/compare_watermark_benchmarks.py `
  --enforce-timing-gates `
  snow_draw_engine_qt/build-perf/watermark-baseline.csv `
  snow_draw_engine_qt/build-perf/watermark-candidate.csv
```

The defaults are 10 warmups and 300 measured samples; strategy-selection runs are repeated five
times. `--suite renderer` selects direct
`renderWatermark` measurements. `--suite workflow` selects persistent commits, coalesced preview
bursts, preview-plus-widget-paint, and matched watermarked/no-watermark runtime exports.

## Scenario interpretation

The renderer suite separates these costs:

- Hidden and empty watermark early exits.
- Cold shaping, physical-unit rasterization, and tint construction.
- Warm cache reuse at 1080p, 4K, and logical 1080p at DPR 2.
- Dense tiling and a small clipped watermark area on a 4K surface.
- Tint-only color/opacity edits and cache-preserving angle edits.
- Text and font-size changes that rebuild shape/unit caches, plus gap-only placement reuse.
- UTF-8/CJK shaping, segmented maximum-length units, and pathological glyph fallback.

The workflow suite separates configuration cost from presentation cost. It includes steady widget
paint, coalesced preview mutation, persistent commit, render-area movement, camera movement,
same-thread multi-canvas rendering, and matched watermarked/no-watermark runtime exports. The two
hidden export rows are controls for the corresponding watermarked export rows; the comparison
script reports their visible-minus-hidden incremental cost rather than subtracting individual
timing samples.

Each scenario validates its functional contract before producing a result. Unexpected visibility,
cache rebuilds, renderer path selection, preview coalescing, or export failures cause a nonzero exit
code. Timing values do not cause failures.

## CSV format v4

CSV format version 4 uses stable scenario names. It records logical and physical dimensions, DPR,
render-area dimensions, watermark parameters, batch size, full timing distributions, throughput,
an output checksum, rendered logical/device bounds, and the following
per-sample renderer diagnostics:

- Render calls and early exits.
- Shape and physical-unit hits/misses, cache evictions, and accounted cache bytes.
- Tint and compact repeat-cell builds.
- Sparse batches, submitted/culled fragments, segmented chunks, dense fills, and glyph fallback draws.
- Selected strategy, fragment coverage, and shape/raster/tint/placement/composition phase timings.

Environment columns record the resolved font, Qt version, Windows version, CPU architecture,
compiler, and build type. Generated CSV files are build artifacts and should not be committed by
default. `scripts/compare_watermark_benchmarks.py` accepts a format-v2, format-v3, or format-v4 baseline and
requires a format-v4 candidate. It validates exactly one decoration render and at most one dense fill per
sample, checks matched visible/hidden exports, and reports p50 deltas plus visible-minus-hidden export overhead.
The optional timing gate uses the recorded-machine policy: warm renderer p50 at 40% of baseline,
full 1080p preview p50 at 50%, 1080p export watermark overhead at 35%, 4K export watermark
overhead at 40%, and maximum-text segmented p50 below 5 ms.

Retain an adaptive strategy only when its median-of-run p50 is at least 5% faster than sparse,
its p95 is no more than 5% worse, and it wins at least four of five runs. Otherwise keep the
simpler sparse strategy for that workload.

For useful before/after data, keep the machine, power profile, Qt version, compiler, Release flags,
font, platform backend, scenario, warmups, and measured sample count unchanged. Use medians for the
primary comparison and inspect p95/p99 and standard deviation for instability. There are
intentionally no machine-dependent pass/fail thresholds.

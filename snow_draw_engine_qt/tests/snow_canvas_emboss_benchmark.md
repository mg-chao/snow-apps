# Emboss filter performance results

Measured on 2026-09-15 against the implementation at `b0379f02`.

## Change

The original emboss pixel kernel repeatedly resolved diagonal sample rows and evaluated
`clamp(0.5 + strength * channelDelta / 765, 0, 1)`. The optimized kernel resolves
sample rows once per scanline (or sparse span), and computes the response for all 1,531
possible RGB-sum differences once per dispatch. Workers share this immutable 12,248-byte
stack table. Per-pixel work retains RGB-sum extraction, lookup, alpha multiplication,
rounding, and existing coverage blending. No image-sized intermediate, persistent cache,
new thread, or architecture-specific instruction is added.

The original floating-point operation order is preserved when building the table. Because
the response is in [0, 1], rounding its product with alpha already produces a channel in
[0, alpha]. All full-image, rectangular, regional, dense-mask, and sparse-mask paths use
the same response calculation and preserve the existing immutable source handling.

The benchmark now includes emboss scenarios and explicitly imports Qt's offscreen platform
plugin for static Qt builds, using the existing repository helper.

## Environment and method

- AMD Ryzen 9 5950X, Windows x64, Balanced power plan.
- MSVC 14.51, Qt 6.11.1 (installed static kit), Rust 1.97.1.
- `windows-msvc-performance`, Release, release optimization and IPO enabled.
- Local CMake overrides: `VCPKG_MANIFEST_INSTALL=OFF` to use installed dependencies,
  and `SNOW_APPS_BUILD_SNOW_SHOT=OFF` to link the drawing engine's own Rust library.
  The unmodified combined Snow Shot Rust bundle failed to link this benchmark because
  its FFmpeg symbols were unresolved. No repository build defaults were changed.
- Qt platform: `offscreen`; no visible benchmark windows are required.
- Three alternating baseline/optimized rounds per scenario, 20 warmups and 200 timed
  iterations per round. Tables report the median of the three per-run p50 and p95 values.
- Identical benchmark source, inputs, compiler, and configuration for both binaries.
  Only the emboss implementation differs. Input restoration is outside kernel timings;
  normal in-place detachment/copy costs inside `apply()` remain included.
- Only the nine emboss scenarios were run. Timings are machine-specific; renderer results
  include scene replay and composition, so their gains are smaller than kernel-only gains.

## Results

All times are milliseconds; speedup uses p50. Every scenario improved across the aggregate
measurement, and all sampled output checksums matched across all six runs.

| Scenario | Before p50 | After p50 | Speedup | Before p95 | After p95 |
|---|---:|---:|---:|---:|---:|
| `kernel_emboss_1920x1080` | 4.5129 | 2.5476 | 1.77x | 5.6841 | 3.3324 |
| `kernel_emboss_256x256` | 0.6481 | 0.2111 | 3.07x | 0.8635 | 0.2427 |
| `kernel_emboss_3840x2160` | 21.1385 | 10.3027 | 2.05x | 23.0640 | 12.3587 |
| `kernel_emboss_one_thread_1920x1080` | 21.7937 | 7.9504 | 2.74x | 25.8892 | 9.2342 |
| `kernel_masked_emboss_opaque_1920x1080` | 3.0210 | 1.0243 | 2.95x | 4.1384 | 1.4940 |
| `kernel_masked_emboss_partial_1920x1080` | 3.5846 | 1.7549 | 2.04x | 5.3223 | 2.7855 |
| `renderer_full_emboss_1080p` | 6.7666 | 4.6872 | 1.44x | 8.8961 | 6.1509 |
| `renderer_local_emboss_4k` | 0.8891 | 0.4810 | 1.85x | 1.0739 | 0.5405 |
| `renderer_pen_append_emboss_dpr2_4k` | 0.6248 | 0.4742 | 1.32x | 0.8053 | 0.6429 |

Raw CSV files and logs are retained under `build/windows-msvc-performance/emboss-results/`
and `build/windows-msvc-performance/emboss-*.log`; generated results are not committed.

## Correctness and validation

- Added a deterministic reference test covering all 1,531 channel differences and all 256
  alpha values at strengths 0, 0.001, 0.05, 0.1, 0.37, 0.5, and 1: 2,743,552 exact comparisons.
  It passed against both the original and optimized implementations.
- Existing filter tests cover edge clamping, DPR support, immutable sources, rectangular,
  regional and masked rendering, partial opacity, batching, and rendering integration.
- `snow-canvas-filter-render-tests` and `snow-canvas-retained-filter-paint-tests` passed
  with the optimized Release binary. No full test suite was run.
- Strict Release compilation passed. `clang-format --dry-run --Werror` passed for all
  modified C++ files, and `git diff --check` passed. Clang-tidy was not enabled in this preset.

## Reproduce a focused run

From the repository root:

```powershell
. ./scripts/snow-build-environment.ps1
Set-SnowBuildEnvironment -Preset windows-msvc-performance | Out-Null
cmake --preset windows-msvc-performance -DVCPKG_MANIFEST_INSTALL=OFF -DSNOW_APPS_BUILD_SNOW_SHOT=OFF
cmake --build --preset build-windows-msvc-performance --target snow-canvas-filter-benchmark snow-canvas-filter-render-tests
$env:QT_QPA_PLATFORM = 'offscreen'
$benchmark = './build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-filter-benchmark.exe'
& $benchmark --scenario kernel_emboss_one_thread_1920x1080 --warmup 20 --iterations 200 --csv ./build/windows-msvc-performance/emboss.csv
& $benchmark --scenario renderer_full_emboss_1080p --warmup 20 --iterations 200
ctest --preset test-windows-msvc-performance -R '^snow-canvas-(filter-render|retained-filter-paint)-tests$'
```

Use `--list` to find the other emboss scenarios. For an A/B comparison, build the benchmark
additions with the original emboss implementation, save that executable, then rebuild the
optimized implementation. Alternate both executables on the same machine while keeping
the configuration and iteration counts identical.

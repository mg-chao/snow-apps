# Scrolling image replay benchmark

This benchmark runs the application's frame pool, bounded bridge mailbox, adaptive capture
cadence, Rust stitching session, preview patch generation, Qt queued delivery, and thumbnail
widget. Its source is an in-memory image and its paint destination is a QImage. It forces Qt's
offscreen platform and never shows a desktop window. Native capture, display color restoration,
window placement, GPU readback, and compositor presentation are outside this measurement.

## Run

From the repository root:

```powershell
./scripts/run-snow-shot-scrolling-image-performance.ps1
./scripts/run-snow-shot-scrolling-image-performance.ps1 -DetailedTiming
```

Both commands configure `windows-msvc-performance` and build only
`snow-shot-scrolling-image-performance-benchmark` and its dependencies in Release. The runner
restores the process environment, resolves Qt through the repository build environment, and puts
artifacts beneath `build/windows-msvc-performance/scrolling-image-performance/coarse` or `detailed`.
It does not invoke the older scrolling benchmark runner or any UI Automation test.

Parameters: `-Image`, `-ViewportHeight` (1600), `-StepPx` (25), `-ScrollFps` (30), `-MaxSteps` (-1
for the entire image), `-OutputDirectory`, and `-DetailedTiming`. The executable accepts the
corresponding `--image`, `--viewport-height`, `--step-px`, `--scroll-fps`, `--max-steps`, and
`--output-dir` options. `--max-steps 0` measures the initial viewport only.

The default fixture is `snow_shot/test-imgs/scrollscreenshot-test.png`, a 3840 x 21944 PNG.
It remains a local fixture; this change does not add the large image to version control.
One pass covers 814 movements: 813 at 25 pixels followed by 19 pixels. The initial viewport is
painted before the 27.133333334-second motion clock starts. There are no discarded warmup passes.

## Timing semantics

The source position advances from absolute monotonic deadlines, independently of processing.
The source samples at the application's adaptive 1-30 FPS capture rate. Late samples capture the
current position rather than replaying missed ticks. The source queue holds three frames and
drops the oldest on overflow; the production mailbox holds two pending frames, and the stitch
pool holds six buffers. Missing source positions and source/mailbox drops are reported separately.
After the final position is processed, input is stopped and queued work drains. Initialization
and final draining each have a 30-second watchdog.

`results.json` includes the build configuration, profiling mode, source/workload dimensions,
initialization costs, per-frame correlation IDs and offsets, dispositions, queue peaks, capture
rates, stitch outcomes, output dimensions, checksums, and stage distributions. Durations are
monotonic nanoseconds; the console converts percentile summaries to milliseconds. Missing stage
samples mean that the stage was not executed or not compiled in, not that it took zero time.
Every distribution reports count, total, mean, nearest-rank p50/p95/p99, and maximum.

Coarse timings cover source preparation and waits, pool acquisition and copying, mailbox and Qt
queue delays, stitching, preview layout/render/wrapping, thumbnail updates and painting, and
capture-to-paint/source-motion-to-paint latency. Source-motion latency measures output freshness;
it includes the time between the source position changing and capture sampling that position.
Initial-frame samples are retained and can be identified by frame ID 1.
Per-frame capture, source-motion, and paint timestamps share an origin at the first capture;
unpainted frames have a null paint timestamp. Stitch outcome IDs match `SnowStitchFrameEvent`
in `snow_stitch_images.h`. Achieved capture and thumbnail rates exclude the initial frame and
use scrolling plus drain time as their denominator.

Input preflight uses a scaled Qt PNG read to validate compressed data before the Snow Image
decode. This extra initialization pass avoids an observed native codec fail-fast on malformed
PNG input in the current Windows kit. It is reported as `png_validation_ns`, retains no second
full-size source, and does not affect scrolling timings. Snow Image decodes the retained source
once. Revisit this extra validation pass when the native codec error path is repaired.

`SNOW_SHOT_SCROLLING_PERF_INSTRUMENTATION` is private to the replay targets. Normal application
builds do not execute these timers. Configure `-DSNOW_SHOT_SCROLLING_PERF_DETAIL=ON` to add nested
C++ thumbnail scopes and the Rust `perf-instrumentation` feature through the combined FFI bundle.
That build tree's Rust archive is instrumented; turn the option OFF and rebuild before collecting
coarse baselines or using that tree for ordinary app performance measurements.

Rust records current-thread inclusive scopes for freezing, equality checks, reference preparation,
grayscale, similarity maps, feature extraction, descriptor matching, candidate scoring/refinement,
region updates, canvas composition, reference synthesis, preview scaling, and total push time.
Parallel phases are timed around their joins. Do not sum nested or overlapping scopes into total
latency. The optional C snapshot API is gated by `SNOW_STITCH_PERF_INSTRUMENTATION`; existing C
structures and entry points retain their layouts and signatures.

`thumbnail.png` is the final painted widget (including controls and its visible viewport).
`preview.png` is the complete accumulated preview, expected to be 128 x 732 for the default input.
Flattening, checksumming, JSON serialization, and image artifact writing are outside frame timings.
Image artifact writing is reported separately; JSON writing necessarily follows the report it
writes. A full stitched image is not materialized during benchmark measurement.

Failure returns a nonzero exit code with available diagnostics and image artifacts. Causes include
invalid input, pipeline errors, watchdog expiry, missing thumbnail, incomplete source coverage,
or artifact writing failure. Drops and slow timings are measured results, not hardware-dependent
pass/fail thresholds.

## Focused verification

Build `snow-shot-scrolling-image-replay-tests`, `snow-shot-latest-bridge-mailbox-tests`, and
`snow-shot-scrolling-capture-cadence-tests`, then run only those names with CTest. Replay tests
cover absolute scheduling, fractional periods, final partial movement, invalid image/geometry
input, bounded source/mailbox queues and attributed drops, adaptive source sampling, startup and
drain deadlines, cancellation, known vertical/horizontal movement, snapshot pixel equality, stale
generations, preview patch/tile equality, and nonblank offscreen painting. They also check duplicate
early returns, frame correlation, profiling availability, and percentile calculations. Run the
affected `snow-stitch-images` and
`snow-stitch-images-c` crate tests with profiling disabled and enabled. The two real-fixture
benchmark runs validate complete 3840 x 21944 coverage without changing stitching heuristics.

The implementation prioritizes production-path fidelity, timing attribution, and testability.
The source adapter and timing hooks add no dependency, setting, or migration requirement.

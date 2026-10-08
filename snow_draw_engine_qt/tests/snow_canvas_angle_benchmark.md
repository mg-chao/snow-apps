# Angle annotation performance and validation

## Reproduction

Build benchmarks only with the Release performance preset:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-canvas-angle-benchmark snow-canvas-distance-benchmark
& build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-angle-benchmark.exe 100 3 10000 > build/angle-final.csv
& build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-distance-benchmark.exe 100 3 > build/distance-final.csv
```

The angle arguments are measured iterations, independent repeats, and maximum existing
annotation count. Defaults are 300, 3, and 10,000. Each scenario warms up for 30 iterations;
document population and first text measurement are outside the measured interval. The
reported run uses 100 measured iterations per repeat, giving 300 samples per scenario.
Run the two executables sequentially after builds and tests finish.

The offscreen Qt widget is 1280 by 720 at device pixel ratio 1. Inputs go through the widget
and its Rust engine. Preview and endpoint samples change the displayed integer angle;
the same-label scenario changes geometry while retaining the formatted text. Wheel
samples alternate +1 and -1 degree. Assertions verify that wheel and endpoint scenarios
actually change the newly created angle's second arm and preserve its other two points.
Quick selection is disabled for the Angle tool during benchmark setup so creation clicks
over a populated scene start a draft. The subsequent Select-tool interactions retain the
ordinary selection policy.
Population counts are checked against each import result. Interaction assertions export
only the selected owner and its generated label, avoiding the existing 16 MiB full-session
serialization limit when a stress document includes all import undo/redo records. The
benchmark does not change that persistence limit.

Existing populations contain 100, 1,000, or 10,000 angle annotations, each with a generated
label: the largest case therefore has 20,000 document elements. Offscreen populations are
outside the viewport. Visible populations repeat a 200-position grid, so the 10,000-angle
case deliberately overdraws each position 50 times. This is a dense paint stress case.
Stroke widths 2, 8, 24, and 72 are measured separately without an existing population.

Input time covers synchronous input handling, measurement, scene updates, and scheduling.
The subsequent event-processing interval is reported separately as paint time. Qt may
coalesce update requests, so that interval is not a forced complete frame on every sample.
The `angle-visible-paint` scenario calls `repaint()` synchronously and measures complete
warm frames. CSV files contain p50 and p95 in milliseconds for each independent repeat.

## Changes verified by the benchmark

- Measurement layout requests are transaction-local. Unchanged formatted labels reuse
  measured text, and unrelated owners are not rewritten during angle interactions.
- Ordinary drafts reuse committed scene order and sparse spotlight state. Spatial preview
  candidates and owner lookups avoid full-document scans.
- Read-only viewport queries temporarily borrow the editor with the requested view instead
  of cloning the editor and all label caches. Model paint ranks are cached for constant-time
  element-state queries.
- Smart Erase synchronization extracts only its own committed regions and previews. Its
  sparse membership cache follows scene deltas, so angle inputs do not enumerate unrelated
  generated labels or walk the document solely to discover that no Smart Erase region exists.
- Short owned paths reject chunks directly by bounds. Label clipping is skipped only after
  a conservative, cached stroke intersection check; stroke width, geometry, label bounds,
  zoom, and device pixel ratio participate in invalidation.
- Text layouts remain cached; layout memo entries are removed with their owners. Path and
  clip caches are bounded by live display items rather than pointer-event history.

Focused counter regressions verify that moving an ordinary draft in a 4,096-angle document
does not rebuild document order or enumerate spotlight or Smart Erase candidates. A warm
128-angle wheel regression verifies that unchanged formatted labels retain their measured
sizes and do not request unrelated layouts. A 10,000-angle regression with warm, visible
measured-label overrides verifies that a draft move or selected-angle wheel change emits
exactly two changed scene items and one path geometry update. Unrelated geometry retains
its cached `Arc` pointer and revision, and wheel changes request layout only for their owner.
Render tests cover label-clip invalidation and short horizontal and vertical paths; the
exported angle preview is compared pixel-for-pixel before and after the rendering
optimizations.

## Measurement environment

Measured on 2026-10-09 using Windows 11 Pro build 26300, an AMD Ryzen 9 5950X
with 24 logical processors available, and 64 GiB RAM. The toolchain is CMake 4.2.3,
MSVC 19.51.36252.0, Rust 1.97.1, and the repository's static Qt 6.12.0 build.
These are offscreen software-rendering measurements, not GPU or compositor timings.

## Results

All 102 angle rows and 60 distance rows completed with the benchmark assertions enabled.
The following values are the median of the three independent per-repeat p50 or p95 values;
they are not percentiles pooled across all samples. Units are milliseconds. The raw results
are `build/angle-final.csv` and `build/distance-final.csv` in the build workspace.
The machine was shared with existing application processes. Timing variance was substantial
in the densest case: complete-frame p50 ranged from 400.038 to 714.364 ms across repeats.

### Single-angle input and stroke widths

| Scenario | Width | Input p50 | Input p95 | Event processing p50 | Event processing p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Preview, changing label | 2 | 0.0887 | 0.2705 | 0.2370 | 0.6756 |
| Preview, changing label | 8 | 0.0690 | 0.2272 | 0.1895 | 0.5429 |
| Preview, changing label | 24 | 0.0886 | 0.2609 | 0.4811 | 0.8911 |
| Preview, changing label | 72 | 0.0989 | 0.3528 | 0.7378 | 1.7625 |
| Preview, unchanged label | 2 | 0.0519 | 0.1207 | 0.1401 | 0.3413 |
| Selected wheel adjustment | 2 | 0.0961 | 0.2628 | 0.2283 | 0.4535 |
| Endpoint editing | 2 | 0.0993 | 0.2940 | 0.2933 | 0.7482 |

### Offscreen populations

All rows use stroke width 2. The active angle remains visible.

| Existing angles | Scenario | Input p50 | Input p95 | Event processing p50 | Event processing p95 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100 | Preview, changing label | 0.0812 | 0.2388 | 0.2062 | 0.4480 |
| 100 | Preview, unchanged label | 0.0533 | 0.1154 | 0.1363 | 0.2760 |
| 100 | Selected wheel adjustment | 0.1140 | 0.2540 | 0.2933 | 0.7901 |
| 100 | Endpoint editing | 0.0935 | 0.2842 | 0.2770 | 0.5962 |
| 1,000 | Preview, changing label | 0.0890 | 0.1988 | 0.1859 | 0.4119 |
| 1,000 | Preview, unchanged label | 0.0560 | 0.1319 | 0.1474 | 0.3572 |
| 1,000 | Selected wheel adjustment | 0.1232 | 0.3807 | 0.2487 | 0.5612 |
| 1,000 | Endpoint editing | 0.1149 | 0.3102 | 0.2975 | 1.0264 |
| 10,000 | Preview, changing label | 0.1502 | 0.5312 | 0.1845 | 0.4398 |
| 10,000 | Preview, unchanged label | 0.0522 | 0.2278 | 0.1504 | 0.8471 |
| 10,000 | Selected wheel adjustment | 0.3422 | 0.9896 | 0.2706 | 1.0620 |
| 10,000 | Endpoint editing | 0.4948 | 4.1906 | 0.3496 | 1.2626 |

### Visible populations

All rows use stroke width 2. The complete-frame rows force a synchronous `repaint()`;
the other rows report the subsequent event-processing interval.

| Existing angles | Scenario | Input p50 | Input p95 | Paint/event p50 | Paint/event p95 |
| ---: | --- | ---: | ---: | ---: | ---: |
| 100 | Preview, changing label | 0.3591 | 0.7850 | 0.7953 | 1.4646 |
| 100 | Preview, unchanged label | 0.3298 | 0.5349 | 0.6913 | 1.1547 |
| 100 | Selected wheel adjustment | 0.4250 | 1.1154 | 1.0772 | 1.8519 |
| 100 | Endpoint editing | 0.4259 | 0.9388 | 1.0673 | 1.7659 |
| 100 | Complete frame | — | — | 7.8336 | 11.1398 |
| 1,000 | Preview, changing label | 6.1513 | 14.3663 | 5.8413 | 14.4989 |
| 1,000 | Preview, unchanged label | 5.8434 | 8.9786 | 5.0417 | 6.9402 |
| 1,000 | Selected wheel adjustment | 8.4008 | 13.9631 | 8.3020 | 12.6194 |
| 1,000 | Endpoint editing | 7.1325 | 9.7729 | 6.5600 | 8.7471 |
| 1,000 | Complete frame | — | — | 77.1052 | 100.6220 |
| 10,000 | Preview, changing label | 82.9392 | 161.8840 | 48.3139 | 90.2388 |
| 10,000 | Preview, unchanged label | 68.8254 | 105.8700 | 37.7149 | 50.3212 |
| 10,000 | Selected wheel adjustment | 57.9665 | 73.9939 | 44.1155 | 60.9641 |
| 10,000 | Endpoint editing | 68.0654 | 116.5880 | 49.5788 | 66.8352 |
| 10,000 | Complete frame | — | — | 709.7780 | 1,258.5100 |

Offscreen input remains below 0.5 ms at p50 with 10,000 existing angles. Dense visible
populations remain expensive: the scene still queries, composes, and compares visible
items, even though cached paths are retained and the C bridge receives only changed items.
Imported labels initially use estimated metrics; their measured presentation overrides
remain necessary without rewriting persisted history. Painting 10,000 overlapping angles
and labels also requires substantial software rasterization. These results do not establish
real-time interaction at that extreme visible population.

### Shared-path distance comparison

The existing distance benchmark was rebuilt with the same Rust library and Qt renderer
and run sequentially on the same machine. These are comparable shared-path operations,
but Distance has two-point geometry and a different creation workflow.

| Scenario | Width | Input p50 | Input p95 | Event processing p50 | Event processing p95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Distance creation | 2 | 0.1320 | 0.2397 | 0.2750 | 0.4306 |
| Distance creation | 8 | 0.1231 | 0.2425 | 0.2796 | 0.4677 |
| Distance creation | 24 | 0.1470 | 0.2834 | 0.7285 | 1.1054 |
| Distance creation | 72 | 0.1273 | 0.3277 | 0.8455 | 1.4012 |
| Distance unchanged label | 2 | 0.0365 | 0.0705 | 0.1151 | 0.2762 |
| Distance endpoint editing | 2 | 0.1104 | 0.1981 | 0.2090 | 0.3825 |
| Arrow creation | 2 | 0.0387 | 0.0719 | 0.1210 | 0.2134 |

At 100 visible annotations, a complete Angle frame measured 7.8336 ms at p50 versus
6.2828 ms for Distance. Angle paints an additional directed arc. Zero-population Angle
preview, wheel, and endpoint input are in the same sub-millisecond range as the Distance
operations. The benchmarks have no performance pass/fail threshold; the functional and
counter assertions enforce the intended interaction and incremental-update invariants.

## Validation scope

Run only related tests. The focused Qt/application selection covers angle, distance,
arrow-text, path culling, text render caches, serial-number rendering, all four angle wheel
hosts, palettes, style commits, toolbar layouts, shortcuts, MCP angle contracts, and
translation catalogs. Rust regressions cover geometry, creation, full turns, transforms,
editing, generated labels, history coalescing, session restoration, scene order, cached
model ranks, and cross-viewport read-only queries. Strict scoped Clippy and C++ formatting
are checked separately. No full CTest or Rust workspace suite is required.

All 25 selected Qt/application CTests passed. The final warmed-label wheel-reversal
fix also passed 28 filtered engine angle tests, 48 related history tests, seven editor
measurement tests, and strict editor/engine Clippy. The reversal regression covers two
selected owners and an unrelated owner, exact restoration of generated label data,
zero new measurement requests, session save/restore, earliest undo, and redo.
After rebuilding the final Rust fix and Qt consumers together, angle, distance,
arrow-text, and application angle integration CTests passed again.
The final Smart Erase extraction change passed 29 filtered engine angle tests, five engine
Smart Erase tests, two sparse scene-cache tests, and the 4,096-angle cache counter regression.
Strict editor, scene, and engine Clippy and Rust formatting checks passed.
Snow Shot and the Qt consumers rebuilt successfully with that change; the final angle,
distance, arrow-text, Smart Erase, and application angle integration CTests all passed.
The final 10,000-angle warm-visible patch regression and strict engine Clippy passed
without further production changes, so the benchmark measurements above remain current.

The broad MCP capability snapshot has a pre-existing mismatch: `RecordingToolbarEditor`
exists in the settings renderer code but is absent from the snapshot. Existing distance
snapshot omissions are also outside this change. The dedicated `--angle-only` checker
validates the new entries in both full and mini capability documents, including exact
three-point cardinality, optional `full_turn`, unit enums, precision bounds, and strict
target-specific style schemas. Unrelated snapshot entries are not repaired here.

# Bracket annotation benchmark

Measured 2026-10-29 on Apple M4 (16 GB), macOS 27.0.1, Qt 6.12.0 and Rust 1.97.1. Native arm64 `snow-shot-macos-arm64-performance`, `CMAKE_BUILD_TYPE=Release`, benchmarks enabled. No new dependencies.

## Reproduce

```sh
cmake --build --preset build-snow-shot-macos-arm64-performance --target snow-canvas-bracket-benchmark
build/snow-shot-macos-arm64-performance/snow_draw_engine_qt/snow-canvas-bracket-benchmark 120 > bracket-benchmark.csv
```

Each case warms 20 updates and measures 120 changing pointer positions. Creation measures a bounding-box draft; endpoint, apex and guide cases edit a committed bracket. Input includes synchronous event dispatch and engine updates. A complete frame includes input plus one forced full-widget repaint at 1280 × 720, DPR 1, using Qt’s offscreen platform. This measures native engine and raster painting, excluding the OS compositor and display refresh. `commit_ms` is one final mouse-release transaction, not a percentile.

The visible population uses small brackets on a repeating 50 × 50 grid, away from the edited bracket. All populated brackets are inside the viewport; offscreen populations sit outside it. Full repaints deliberately include all visible strokes, even when only the active bracket changed.

## Results

Times are milliseconds; each cell is p50 / p95.

| Existing | Population | Operation | Input | Complete frame |
| ---: | --- | --- | ---: | ---: |
| 0 | offscreen | creation | 0.006 / 0.007 | 0.091 / 0.096 |
| 0 | offscreen | endpoint | 0.008 / 0.009 | 0.095 / 0.102 |
| 0 | offscreen | apex | 0.007 / 0.009 | 0.094 / 0.101 |
| 0 | offscreen | guide | 0.008 / 0.009 | 0.090 / 0.096 |
| 100 | offscreen | creation | 0.005 / 0.008 | 0.093 / 0.107 |
| 100 | offscreen | endpoint | 0.008 / 0.008 | 0.097 / 0.105 |
| 100 | offscreen | apex | 0.007 / 0.007 | 0.100 / 0.112 |
| 100 | offscreen | guide | 0.008 / 0.008 | 0.096 / 0.103 |
| 100 | visible | creation | 0.034 / 0.039 | 0.638 / 0.668 |
| 100 | visible | endpoint | 0.036 / 0.043 | 0.644 / 0.678 |
| 100 | visible | apex | 0.036 / 0.042 | 0.646 / 0.680 |
| 100 | visible | guide | 0.037 / 0.039 | 0.642 / 0.667 |
| 1,000 | offscreen | creation | 0.005 / 0.006 | 0.090 / 0.095 |
| 1,000 | offscreen | endpoint | 0.008 / 0.008 | 0.097 / 0.099 |
| 1,000 | offscreen | apex | 0.007 / 0.007 | 0.099 / 0.102 |
| 1,000 | offscreen | guide | 0.008 / 0.008 | 0.096 / 0.103 |
| 1,000 | visible | creation | 0.269 / 0.300 | 5.311 / 5.384 |
| 1,000 | visible | endpoint | 0.281 / 0.294 | 5.315 / 5.375 |
| 1,000 | visible | apex | 0.279 / 0.296 | 5.312 / 5.370 |
| 1,000 | visible | guide | 0.279 / 0.303 | 5.314 / 5.388 |
| 10,000 | offscreen | creation | 0.006 / 0.006 | 0.090 / 0.098 |
| 10,000 | offscreen | endpoint | 0.008 / 0.009 | 0.100 / 0.106 |
| 10,000 | offscreen | apex | 0.008 / 0.008 | 0.102 / 0.113 |
| 10,000 | offscreen | guide | 0.008 / 0.009 | 0.097 / 0.100 |
| 10,000 | visible | creation | 5.087 / 5.352 | 56.431 / 56.914 |
| 10,000 | visible | endpoint | 5.257 / 5.574 | 56.792 / 57.238 |
| 10,000 | visible | apex | 5.359 / 5.638 | 56.914 / 57.442 |
| 10,000 | visible | guide | 5.266 / 5.541 | 56.706 / 57.138 |

## Cache behavior and refinement

All 28 cases built exactly **120 path chunks for 120 changed updates**, regardless of population. Only the active bracket’s one chunk was rebuilt. Retained-path reads per case were 840 with offscreen populations, 60,840 with 100 visible brackets, 351,240 with 1,000, and 2,852,040 with 10,000. Repaint-only full-reset reuse counters were zero: unchanged population paths were not republished. The benchmark checks that rebuild counts stay bounded independently of document size.

The engine regression also verifies pointer identity of an unrelated cached path, exactly one active geometry patch per edit, and no scene or geometry patch for an unchanged pointer position. No-op and canceled gestures add no undo operation.

Bracket bounds now evaluate the extrema of four fixed cubic segments directly. They avoid building temporary path chunks and calculating arc lengths; the shared cubic-extrema helper also uses a fixed array instead of allocating a vector. Rendering continues through the existing Rust scene and Qt path caches.

At 10,000 offscreen annotations, the worst input p95 was 0.009 ms and complete-frame p95 was 0.113 ms. At 10,000 visible annotations, input p95 stayed below 5.64 ms, while forcing all 10,000 strokes to repaint took up to 57.45 ms p95. Full-scene raster painting remains the limiting cost in this stress case; this result does not imply 60 FPS for a full repaint of 10,000 visible annotations.

Final transaction times ranged from 0.017–0.400 ms with offscreen populations and reached 6.010 ms with 10,000 visible annotations. These are individual commit samples.

## Focused validation

- Rust: bracket geometry, actual-stroke hit testing, extreme proportions, rotated/mirrored curves, creation, independent controls, crossing and coincident endpoints, cancellation, atomic invalid input, undo/redo, copy/duplication/deletion, group transforms, session serialization, cache locality and no-op updates.
- Qt/application: three control points, guide-only endpoint movement, export exclusion, style alpha/width persistence, toolbar order and both migration paths, remembered tools, assignable shortcuts, shared style bindings, MCP style contracts, and icon states/translations.
- Angle, Distance and Arrow regressions; scoped format/Clippy checks; generated icon/capability checks; application-wide translation extraction and completed en_US/zh_CN/zh_TW catalogs.

Only affected test filters and this benchmark were run. No full test suite was run.

# Background eraser rendering

Rectangle Eraser and Brush Eraser restore original premultiplied RGBA pixels at their
position in scene order. Fully covered spans use row copies; antialiased edges interpolate
all four channels. Restoration never dispatches a current-composite filter kernel. Brush
coverage uses the existing 64-pixel mask atlas, including cached empty tiles, so sparse
strokes do not fetch pristine source tiles outside their actual coverage.

The renderer retains pristine source tiles independently of annotation revisions and
temporary host previews. Hosts can provide `renderOriginalBackground()` and
`originalBackgroundRevision()`; existing hosts inherit their normal background behavior.
Reference rendering shares the widget's mask atlas and retains pristine pixels across
unrelated document edits. Clearing render state invalidates both derived caches.

## Commands

Use a Release performance preset and run only the related targets:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-canvas-eraser-benchmark snow-canvas-filter-benchmark
$env:QT_QPA_PLATFORM = 'offscreen'
build/windows-msvc-performance/snow_draw_engine_qt/Release/snow-canvas-eraser-benchmark.exe --iterations 100
```

The eraser benchmark also accepts `--scenario rectangle|brush|append|dirty64`. It warms
each scenario for three frames, then reports p50/p95 and the last measured frame's
rendering counters. All scenarios use a 1920x1080 logical canvas at DPR 2, a patterned
original image with partial alpha, and an opaque annotation covering the canvas.
`rectangle` restores a 1700x880 logical rectangle. `brush` replays an immutable 4104-point
stroke at width 24. `append` measures an eight-point tail appended to a 4096-point
stroke; preparing and rendering the initial stroke happen outside the timer. `dirty64`
paints a 64x64 logical exposed region on the same physical 4K canvas.

Timings include allocation/filling of the fixture's complete output image, retained plan
resolution, tile composition, and presentation. They exclude constructing the source
pattern. `allocated_bytes` counts pooled rendering scratch allocations; it does not
count the fixture's output image or every temporary `QImage`. Zero in this counter is a
scratch reuse assertion, not a claim of zero heap allocations.

## Controlled comparison method

The 2026-10-02 audit uses Windows 11 Pro build 26200, a reported AMD Ryzen 9 5950X with
24 logical processors available, 64 GiB RAM, and the Balanced Windows power plan. The
performance preset uses MSVC 14.51.36231, Qt 6.11.1, Rust 1.97.1, CMake 4.2.3, `/O2`,
`/GL`, and Release LTCG. Qt renders offscreen; timings run after compiler/linker activity
has ended.

The control takes these five implementation bodies from repository HEAD
`a303e22bff28026fe96a78939a489c7ed6c0c411`: `snow_canvas_renderer.cpp`,
`snow_canvas_filter_render.cpp`, `snow_canvas_filter_tile_cache.cpp`,
`snow_canvas_pen_mask_atlas.cpp`, and `snow_canvas_display_item.cpp`. It compiles them
against the same current headers, ABI, PCH, benchmark objects, and unchanged Release
objects as the new renderer. This compares the common renderer changes; it is not a
complete historical application build and does not isolate toolbar, widget, or Rust
changes.

The isolated control lives under the ignored
`build/windows-msvc-performance/renderer-eraser-audit/` directory. `prepare-baseline.ps1`
copies the HEAD bodies into the generated unity units 2-4 and removes the new restoration
helper include. `compile-direct.ps1` uses each unit's exact recorded
`CL.command.1.tlog` options, replacing only its source, `/Fo`, and `/Fd` destinations.
`lib.exe /LTCG` combines these three objects with the other frozen canvas objects;
`link.exe` uses the benchmark's recorded link response with an isolated canvas library
and output/PDB/LTCG destinations. The response files, baseline HEAD, executable SHA256
hashes, CSVs, and logs remain in this directory for the audit.

`run-comparisons.ps1` runs the four related existing scenarios with `--warmup 10
--iterations 100 --csv <scenario>-<variant>-<round>.csv`, in three rounds ordered
baseline/current, current/baseline, baseline/current. It then runs the four new eraser
scenarios for 100 samples in each of three rounds. It refuses to time benchmarks while
`cl`, `rustc`, `cargo`, or `link` is running, or an active CMake/MSBuild driver is present.
Checks run before and after each process; retained idle MSBuild worker nodes are ignored.
Reported table values are medians of the
three per-run p50 and p95 values, rather than percentiles of pooled samples.

Unrelated Debug/Cargo builds restarted during this audit. Interrupted processes were
discarded; complete processes passed both activity checks. `run-followup.ps1` repeats
the 10K scene for three alternating pairs of 1000 samples. `run-tail-audit.ps1` repeats
the retained-source and Pen Filter append cases at the same sample count and finishes
the remaining eraser rounds. `run-append-final.ps1` makes five alternating append pairs
with 2000 samples per process. All use ten warmups. Their response files, exact options,
completion markers, and raw results are retained in the audit directory.
`run-append-pinned.ps1` runs the separate process-local diagnostic below.

## Measured results

The initial three pairs, with 100 samples per process, produced:

| Existing scenario | Control p50 ms | New p50 ms | Change | Control p95 ms | New p95 ms | Change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Retained Gaussian plan/source, 1080p | 3.9612 | 4.0327 | +1.8% | 4.4386 | 4.9952 | +12.5% |
| Dirty64 mosaic, 1080p | 0.0318 | 0.0308 | -3.1% | 0.0330 | 0.0319 | -3.3% |
| Pen Filter append, DPR 2 / 4K | 0.1163 | 0.1159 | -0.3% | 0.1209 | 0.1270 | +5.0% |
| 10K mostly offscreen / Gaussian, 1080p | 0.4756 | 0.5055 | +6.3% | 0.5901 | 0.6612 | +12.0% |

Longer follow-ups for the apparent >5% changes produced:

| Existing scenario | Pairs × samples | Control p50 ms | New p50 ms | Change | Control p95 ms | New p95 ms | Change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10K mostly offscreen / Gaussian | 3 × 1000 | 0.5084 | 0.5135 | +1.0% | 0.8824 | 0.8541 | -3.2% |
| Retained Gaussian plan/source | 3 × 1000 | 5.4242 | 4.8863 | -9.9% | 6.7539 | 6.4793 | -4.1% |
| Pen Filter append | 3 × 1000 | 0.1182 | 0.1181 | -0.1% | 0.1242 | 0.1316 | +6.0% |
| Pen Filter append | 5 × 2000 | 0.1172 | 0.1192 | +1.7% | 0.1233 | 0.1316 | +6.7% |

The 10K and retained-source changes did not persist in the extended comparisons.
Absolute timings varied substantially: the second 10K control run was 0.6955/1.0114 ms
p50/p95, versus 0.5084/0.8470 and 0.5072/0.8824 in the other control runs. The full
renderer discovers restoration across the scene so that erasers contributing to later
filters' sampling halos preserve their chronological effect.

Pen Filter append leaves a small measured tail concern: the final p95 difference is
8.3 microseconds, while p50 differs by 2.0 microseconds. The final five pairs were:

| Pair | Control p50 ms | New p50 ms | Control p95 ms | New p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0.1197 | 0.1187 | 0.1845 | 0.1817 |
| 2 | 0.1172 | 0.1192 | 0.1252 | 0.1287 |
| 3 | 0.1170 | 0.1283 | 0.1225 | 0.1881 |
| 4 | 0.1163 | 0.1183 | 0.1220 | 0.1234 |
| 5 | 0.1174 | 0.1199 | 0.1233 | 0.1316 |

Both variants have outliers. All work counters match: geometry chunks, rasterized tiles,
source hits/misses, allocated/copied bytes, and filter dispatches. The fifth pair's final
frame reports essentially equal scene replay (105.5/105.3 microseconds) and planning
(2.3/2.2 microseconds). The timing data do not identify a defensible algorithmic fix for
the tail difference. This audit therefore retains the concern and does not establish a
blanket historical application regression limit of 5%.

A separate diagnostic runs both executables on logical processor 23 at AboveNormal
priority. The launching process allowed logical processors 0–23 (`0xffffff`); each
benchmark process was verified to use affinity `0x800000` and the requested priority
immediately after launch. System power settings and other processes were unchanged.
Three alternating pairs, each requesting ten warmups and 2000 samples, produced:

| Diagnostic pair | Control p50 ms | New p50 ms | Control p95 ms | New p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0.1201 | 0.1211 | 0.1252 | 0.1262 |
| 2 | 0.1203 | 0.1220 | 0.1252 | 0.1294 |
| 3 | 0.1207 | 0.1197 | 0.1256 | 0.1252 |
| Median | 0.1203 | 0.1211 | 0.1252 | 0.1262 |

The process-local diagnostic changes p50 by +0.7% and p95 by +0.8%. This supports
scheduling or migration noise as a contributor to the default tail difference; it is
an inference, not a proven root cause. The default +6.7% p95 result and the control's
limited renderer-body scope remain part of the report.

Every measured control/new pair has identical image checksums:
retained source `7458811820260071930`, dirty64 `11025646822929972613`, Pen Filter append
`7093538044335356934`, and 10K scene `13440616747129023004`.

The new eraser scenarios, with three clean 100-sample rounds, produced:

| Eraser scenario | p50 ms | p95 ms | p50 range across runs ms |
| --- | ---: | ---: | ---: |
| Rectangle | 28.8825 | 39.6529 | 28.5735–31.1388 |
| Immutable brush | 29.5668 | 37.6495 | 24.0489–37.4552 |
| Eight-point brush append | 30.9612 | 38.2397 | 26.9997–37.9670 |
| Dirty64 rectangle | 5.0252 | 6.1774 | 4.6344–5.2002 |

The physical 4K output allocation/fill remains inside these timers, including the dirty
case; these are fixture timings, not application input latency. Per-frame counters were
identical in all three rounds:

| Eraser scenario | Working pixels | Restored pixels | Edge blends | Pristine hits | Chunk builds / reuse | Rasterized mask tiles |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Rectangle | 7,864,320 | 5,984,000 | 0 | 120 | 0 / 0 | 0 |
| Immutable brush | 5,505,024 | 997,883 | 40,275 | 79 | 0 / 0 | 0 |
| Eight-point brush append | 5,505,024 | 997,883 | 40,275 | 79 | 1 / 128 | 44 |
| Dirty64 rectangle | 65,536 | 65,536 | 0 | 1 | 0 / 0 | 0 |

All measured eraser frames reported zero pristine misses, zero ordinary effect
dispatches, and zero pooled scratch allocation bytes. Copied bytes were 23,936,000 for
the rectangle, 3,830,432 for either brush case, and 262,144 for dirty64. The rectangle
retained 30 MiB of pristine tiles; brush cases retained 19.75 MiB of pristine tiles and
2,604,508 bytes of mask atlas storage; dirty64 retained one 262,144-byte source tile.
The immutable brush performs no mask or geometry rebuilding, the append rebuilds one
chunk, and the dirty paint uses one 256x256 working tile.

## Cache limits and deterministic checks

Pristine originals share the existing 64 MiB source-cache limit. Viewport and reference
painting share one 16 MiB widget mask atlas; standalone reference requests use a fallback
atlas with the same limit. The existing workspace pool is capped at 128 MiB. A reference
scene also owns its complete output image, approximately `width * height * 4` bytes;
these component limits are not a total process memory limit.

The benchmark asserts no ordinary filter dispatch, no warm pristine misses, no pooled
scratch allocations, no mask work for aligned rectangles, no mask rasterization for
immutable warm brush frames, and at most two rebuilt chunks with at least 63 reused
chunks for the append. Dirty-region working pixels must stay within four 256x256 tiles.
Both retained caches must remain within their byte budgets.

`snow-canvas-eraser-render-tests` covers exact premultiplied output, overlapping erasers,
single-point and duplicate-point preview discs, chronological filter boundaries,
full/tiled/partial paints, fractional camera origins, DPR 1/1.25/2, pristine revision
changes, transparent and alpha-2 blank baselines, and incremental brush geometry.
`snow-canvas-reference-scene-tests` also checks zoom/DPR sampling, exact runtime export
and crop parity, immutable mask and pristine-source reuse after unrelated edits, and
invalidation after explicit clearing.

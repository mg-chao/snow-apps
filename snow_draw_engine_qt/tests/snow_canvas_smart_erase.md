# Smart Erase implementation and validation

Smart Erase is filter enum value 5. Rectangles and pen strokes use the existing
document, selection, transform, serialization, and history mechanisms. Document
normalization fixes their strength at 0.5 and orders them by creation ID below
auto-filter fills and ordinary annotations. Auto-filter categories reject this type.

## Integration contract

Call `SnowCanvasWidget::setBaseImageSources` with original image layers and their
canvas mappings when a capture is installed or replaced. An empty coverage rectangle
means the complete layer; a nonempty coverage rectangle restricts its contribution.
Never supply a composited annotation image. Layers from widgets sharing a runtime
are combined, with identical image/mapping registrations deduplicated.

The runtime owns the coordinator. Stable committed geometry schedules work on a
dedicated pool of two workers. Creation and transform previews display the red
placeholder. Selection alone does not invalidate a result. Source revisions and
geometry identify results; opacity and camera changes reuse them. Cancellation
and completion validation prevent stale jobs from replacing current appearance.
Failed work remains a placeholder and can retry after a later stable transition.

Capture `smartEraseSnapshot()` together with the document session before handing
an export to a worker. Restore it with `restoreSmartEraseSnapshot()` after restoring
the worker's document. The snapshot freezes ready results and placeholders,
including preview geometry. Export does not wait or start reconstruction.

## Reconstruction

The worker uses the existing OpenCV core and imgproc dependencies. A robust affine
RGB surface fit handles flat colors and gradients only when at least 97% of the
sampled outer ring supports it with a small residual. A separate periodic fast
path verifies a short translation throughout the known donor domain. A matching
border alone never authorizes copying an entire unrelated rectangle.

Texture reconstruction uses deterministic multiscale PatchMatch in Lab space.
Donor eligibility is separate from source coverage: a local band starts at
16–96 pixels and expands, up to 512 pixels, only when insufficient full patches
remain. Original unmasked observations provide a fixed low-frequency color and
texture-variation guide. Compatible observations across horizontal, vertical, and
diagonal runs guide the missing background. This is a local heuristic, not semantic
segmentation of photographs or screenshot panels.

Matching gives real boundary pixels more weight than synthesized pixels. Color
and texture compatibility discourage unrelated donors; a reuse penalty discourages
repeated donor locations. Both reconstructed pixels and donor correspondences pass
between pyramid levels. Texture-compatible initialization avoids bias toward flat
patches. Overlapping votes favor compatible background colors and the selected
donor's detail, preserving texture instead of averaging unrelated pixels together.

The selected refinement schedule uses five passes on the coarsest level, three
on intermediate levels, and two on the finest level. A single-level pyramid keeps
five passes. Synthesized-pixel confidence spans 0.15-0.35 for every budget. Search
retains deterministic candidate order and alternating propagation direction.

Patch scoring caches target coverage/normalization and per-iteration donor reuse
penalties, rejects candidates using nonnegative lower bounds, and uses an interior
path without per-pixel coverage checks. Ordered masked row spans avoid rescanning
unused context. Voting reuses two buffers with immutable known pixels, and color
and variation guides share their blurred coverage weights.

Voting uses at most two OpenCV stripes when a level contains at least 32,768
masked pixels. Each pixel retains the same serial accumulation order; patch search
stays serial. This does not change OpenCV's global thread settings or the two-job
coordinator pool.

Donors always come from original known pixels. Pyramid hole masks expand and
coverage/donor masks contract before reduction, so erased colors and unavailable
source pixels cannot become coarse donors. Pyramid construction stops while usable
patches still remain. Rendering clips the result to the rotated rectangle or the
pen's round brush path; known pixels and alpha are preserved.

Donor context is bounded to 512 source pixels around the target. A working region
over 16 Mi pixels fails safely to the placeholder. This bounds individual job
dimensions, not total process memory. Historical results have a 128 MiB cache;
current results and export snapshots can retain additional image memory.

This is patch-based content reconstruction, without a learned generative model.
Results depend on available surrounding content. Flat and repeating screenshot
backgrounds and affine gradients are favorable. Large photographic holes, unique
objects, nonlinear gradients, and ambiguous boundaries can still retain visible
transitions, repeated detail, or synthesized texture artifacts. The algorithm cannot
recover the actual hidden scene. A fully masked source with no donor content keeps
the placeholder.

## Targeted validation

`snow-canvas-smart-erase-tests` covers placeholder shape/color/DPI, asynchronous
coalescing, transform commits, opacity, historical reuse, source invalidation,
failure/retry, deletion, independent export snapshots, reconstruction determinism,
source boundaries, negative origins, rotation, cancellation, general patch search,
and clipping. Its widget test exercises the Rust/FFI path and ordinary filters
above Smart Erase. Related Rust tests cover fixed ordering, strength, undo/redo,
session restoration, conversion undo, auto-filter rejection, and pen-down previews.

`snow-canvas-smart-erase-quality-tests` links the algorithm directly and runs without
a window. Deterministic fixtures cover flat/affine surfaces, horizontal/vertical
background bands next to unrelated panels, diagonal edges, donor diversity,
texture variance, exact periodic backgrounds, holes touching source edges,
masked-object color independence, and unchanged outside pixels. Additional fixtures
cover large diagonal boundaries, thin multi-span strokes, source-edge holes,
rotated masks at fractional mappings, alpha preservation, single-level pass budgets,
exhaustive versus bounded scoring, and byte-identical serial/parallel voting.

Run only the related CTest selection:

```powershell
ctest --preset test-windows-msvc-debug -R '^snow-canvas-smart-erase(-quality)?-tests$' --output-on-failure
```

## Reproducible performance measurements

Build `snow-canvas-smart-erase-benchmark` using `windows-msvc-performance` (Release).
The benchmark links the algorithm directly; its geometry helpers also require the
Rust engine target. In a Snow Shot workspace, the shared Rust bundle propagates its
native FFmpeg dependencies to these standalone consumers.

The optional positional output directory still saves before/after crops, and now
also saves complete `*-result.png` images for comparison. Supported options:

- `--scenario NAME` (repeatable): select fixtures; omitted means all fixtures.
- `--warmup 1 --repeat 7`: warmup and sample counts (the defaults).
- `--schedule 532|533|544|555`: private coarse/intermediate/fine pass budgets.
- `--serial-voting` / `--parallel-voting`: compare voting implementations.
- `--jobs 2`: time completion of two simultaneous reconstructions.
- `--reference DIRECTORY`: compare with saved baseline result images; exit nonzero
  if any quality gate fails. This is separate from timing and memory qualification.

CSV output contains each sample's reconstruction time, working dimensions, masked
pixel count, chosen path (0 empty, 1 surface, 2 periodic, 3 patches), per-level pass
counts, aggregate search/voting times, preparation/guide timings, and process peak
working bytes (Windows; zero on other platforms). Summaries report median and
nearest-rank p95; with seven samples, p95 is the largest observation. Input creation,
image saving/comparison, and process startup are outside the timed interval.
Application calls do not collect or log these diagnostics.

Preserve a baseline executable using the same extended benchmark harness before
changing reconstruction. `scripts/run-smart-erase-perf.ps1 -Baseline <path>` runs
one warmup and seven measured iterations in fresh processes, rotates variant order,
and records executable hashes. It deliberately omits PNG output/comparison so image
analysis buffers cannot distort the reconstruction memory measurements. Use
`-Schedules 532 -Jobs 2` for the concurrent comparison and `-ParallelVoting` for
two-stripe voting. Keep baseline and candidate on the same Release toolchain.

### Schedule selection (2026-09-20)

All four schedules passed the existing and expanded quality fixtures. Comparison
against the original five-pass baseline blurs RGB with sigma 2 source pixels before
computing Lab ΔE76 inside the mask: mean ≤1.5 and p95 ≤4. High-frequency RGB RMS
energy must stay within 85–115% of baseline. The selected 5/3/2 schedule had maximum
mean ΔE 0.668, maximum p95 ΔE 1.760, and texture ratios 0.914–1.026 across the
nonrepeating fixtures. Flat and periodic behavior remains covered by exact tests.
Native-resolution crops were also inspected; existing synthesis artifacts remain.

Geometric-mean latency across eight nonrepeating cases, with serial voting:

| Version / schedule | Geometric mean ms |
| --- | ---: |
| Original implementation | 1175.1 |
| Optimized 5/5/5 | 678.8 |
| Optimized 5/4/4 | 580.8 |
| Optimized 5/3/3 | 469.4 |
| Optimized 5/3/2 (selected) | 390.4 |

The selected schedule is 16.8% faster than 5/3/3, beyond the 3% tie threshold.
Selected serial results isolate the scoring/span/budget improvements from threading:

| Nonrepeating fixture | Original median ms | Optimized median ms | Speedup | Optimized p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 192 × 96 rectangle | 369.1 | 131.8 | 2.80× | 134.1 |
| 384 × 256 rectangle | 2537.8 | 855.5 | 2.97× | 919.9 |
| 768 × 512 rectangle | 9921.8 | 2574.0 | 3.85× | 2761.9 |
| 2400-pixel brush span, width 20 | 1998.4 | 846.2 | 2.36× | 908.4 |
| Thin diagonal stroke | 376.0 | 167.9 | 2.24× | 174.3 |
| Source-edge rectangle | 269.5 | 88.7 | 3.04× | 90.6 |
| 1.25× physical scale | 717.9 | 210.6 | 3.41× | 218.4 |
| 2× physical scale | 2691.5 | 701.2 | 3.84× | 881.3 |

Peak working set in the serial schedule study changed by -0.75% to +0.64%, within
the 10% limit. These are local development observations on an AMD Ryzen 9 5950X
with 12 cores / 24 logical processors visible, MSVC 14.51, Qt 6.11.1 and OpenCV
4.12.0. Other builds ran on the machine during portions of the work, so absolute
latencies vary; preserve individual samples and compare nearby runs.

### Parallel voting qualification

Separate serial/parallel runs used one warmup and seven samples for medium/large
rectangles, the long stroke, and 2× physical scale. Single-job voting-stage medians
improved by 31.5%, 42.3%, 29.0%, and 32.8%, respectively. Two-job completion-time
changes ranged from an improvement to a worst regression of 4.23%, below the 5%
limit. The other workspace's concurrent OpenCV build especially affected absolute
timings in this phase; these samples establish the local adoption gate rather than
a throughput guarantee. Parallel voting is enabled by default after these checks.

For two simultaneous large rectangles, peak working set was 500,842,496 bytes in
the baseline and 504,033,280 bytes with the selected parallel implementation
(+0.64%). The corresponding long-stroke values were 938,700,800 and 939,294,720
bytes (+0.06%). Both stay below the 10% peak-memory limit. Serial/parallel pixel
equivalence is also checked by the deterministic quality tests.

The final-default fast-path comparison also used one warmup and seven samples.
Original/optimized medians in milliseconds were 2.893/3.013 (small texture),
8.567/7.474 (4K text), 7.299/7.045 (8K text), 34.473/34.602 (large repeating
region), and 112.497/108.598 (long repeating stroke). Every case met the limit of
no regression exceeding the larger of 10% or 2 ms. The final default configuration
also passed all thirteen saved-image comparisons.

### Integration validation and local dependency limitation

Release builds of the benchmark, Smart Erase functional/quality tests, filter-render
tests, and screenshot-export-service tests passed strict compilation. Only those
four related CTest cases were run; the full suite was not run. Changed C++ files
passed clang-format checks.

The export executable initially failed before main with Windows status 0xc0000142.
A loader trace located the already documented Release libde265 1.0.16 crash in
`init_scan_orders`, called from HEIF's DLL initializer. The staged DLL matched the
installed package's hash. The existing local Release rebuild from the same source
(`/O2 /Ob1 /d2SSAOptimizer-`) was copied into the generated test directory; the
unchanged export executable then passed. The installed package and codec source
were not modified. Dependency staging can restore the broken original DLL, so this
is an environment qualification, not a codec fix included in Smart Erase.

The nonrepeating fixtures use deterministic waves/gradients and controlled texture
boundaries, not a corpus of photographs. Results do not establish performance or
visual parity with Photoshop on arbitrary photos. Donor context, resolution, mask
safety, placeholder behavior, caching, export snapshots and the two-job coordinator
pool are unchanged.

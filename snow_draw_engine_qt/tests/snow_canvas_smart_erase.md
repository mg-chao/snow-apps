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

After both fast paths reject the input, reconstruction crops its working buffers
to the bounding rectangle of the hole and eligible donor domain, with a margin
equal to the guide radius plus the three-pixel patch radius. The original mapping,
hole mask, output crop and source resolution remain unchanged. Color conversion,
guides, pyramid construction, matching and voting use this smaller region. Source
composition, mask preparation and fast-path detection still use the original ROI.

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
- `--policy default|reference|crop`: selected default, the pre-cropping 5/3/2
  implementation, or explicitly enabled context cropping. `--schedule` can still
  override the pass budgets for either policy.
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
Additional columns report cropped dimensions and each level's dimensions and
search/voting times. All levels in the selected policy retain broad patch search.

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

### Context cropping and restricted-search qualification (2026-09-20)

This study starts from commit `0fea271e`, including the earlier optimized 5/3/2
search and parallel voting. The selected change is **context cropping only**.
The four adaptive search policies were implemented and measured, but rejected by
the agreed visual gates; their restricted search, displacement transfer and
one-pass refinement code are not enabled or retained in the library.

The experimental policies permitted broad search up to 32,768 or 65,536 masked
pixels and 512 K working pixels, with one or two restricted passes on finer
levels. Restricted passes evaluated transferred donor displacements, their 3 x 3
neighborhoods, propagation candidates and nearest valid donors without global
random search. All policies kept the existing surface and periodic paths.

One warmup and seven samples per fixture, in fresh processes with rotated policy
order, produced the following geometric means across eight nonrepeating cases:

| Policy | Geometric mean ms | Image-gate outcome |
| --- | ---: | --- |
| Preserved baseline | 383.50 | Reference |
| Context cropping | 309.96 | Pass |
| 32 K / one restricted pass | 231.64 | Reject: large rectangle and diagonal stroke texture |
| 32 K / two restricted passes | 278.07 | Reject: diagonal stroke texture |
| 64 K / one restricted pass | 242.38 | Reject: large rectangle and diagonal stroke texture |
| 64 K / two restricted passes | 281.47 | Reject: diagonal stroke texture |

The rejected one-pass variants had texture-energy ratios of 1.474 on the large
rectangle and 1.468 on the diagonal stroke. Two passes reduced the diagonal ratio
to 1.278, still beyond the 1.15 limit. Native-resolution inspection also showed
additional directional/block artifacts in one-pass rectangular fills. No gate
was relaxed to admit these policies.

Cropping passed all thirteen saved-image comparisons against the preserved
baseline. Maximum smoothed mean Lab delta was 0.478, maximum p95 was 1.538,
and texture-energy ratios were 0.974-1.078. All five periodic fast-path outputs
were byte-identical. Native-resolution crops of medium/large rectangles, long
and diagonal strokes, source-edge holes and 2x scale were inspected. Existing
synthesis seams/artifacts remain; cropping changes fine texture placement.

A separate final-default qualification reran all thirteen fixtures, alternating
baseline and candidate order, with no image I/O in the measurements:

| Nonrepeating fixture | Baseline median ms | Cropped median ms | Cropped p95 ms | Baseline / cropped peak MiB |
| --- | ---: | ---: | ---: | ---: |
| 192 x 96 rectangle | 126.61 | 106.17 | 109.74 | 58.4 / 36.3 |
| 384 x 256 rectangle | 685.90 | 618.90 | 645.31 | 169.1 / 78.2 |
| 768 x 512 rectangle | 2440.75 | 2195.64 | 2367.18 | 254.6 / 133.1 |
| 2400-pixel brush span | 987.25 | 804.25 | 873.92 | 475.2 / 159.9 |
| Thin diagonal stroke | 194.44 | 144.13 | 171.22 | 121.3 / 94.3 |
| Source-edge rectangle | 92.59 | 89.26 | 98.03 | 39.8 / 29.8 |
| 1.25x physical scale | 214.20 | 188.71 | 194.01 | 78.1 / 44.1 |
| 2x physical scale | 689.23 | 594.18 | 890.51 | 164.2 / 76.4 |

The medium/large/long-stroke geometric-mean speedup is **1.15x**, below the 2x
target. Across all eight nonrepeating cases it is 1.16x. The earlier policy matrix
showed 1.24x across those eight cases; absolute timings varied between runs on the
shared development machine. Large rectangles still take about 2.2 seconds. The
2x-scale p95 also varied upward despite its improved median; these observations
are not a tail-latency guarantee.

The final fast-path baseline/candidate medians were 2.441/2.466 ms (small texture),
7.145/6.900 (4K), 7.085/6.887 (8K), 32.906/33.006 (large repeating region), and
109.051/104.308 (long repeating stroke), within the larger-of-10%-or-2-ms gate.
Every nonrepeating median improved. Final single-job peak-memory changes were
within the 5% regression gate; textured cases used 22-66% less process peak memory.

The long stroke's expensive reconstruction buffers shrink from 3441 x 1104 to
2515 x 178 pixels (88.2% less area); the large rectangle shrinks from 1792 x 1080
to 998 x 742. Initial source composition and fast-path preparation still use the
original region, and full-resolution patch search still dominates large holes.

Stage timings from the median-latency samples (baseline / cropped, milliseconds)
show where the savings occur. Initialization and other unlisted work account for
the remainder of each total:

| Fixture | Cropped dimensions | Search ms | Voting ms | Guide/pyramid ms |
| --- | --- | ---: | ---: | ---: |
| Medium rectangle | 614 x 486 | 416.33 / 428.97 | 86.71 / 89.33 | 78.33 / 25.94 |
| Large rectangle | 998 x 742 | 1681.67 / 1600.99 | 363.91 / 313.16 | 142.23 / 69.82 |
| Long stroke | 2515 x 178 | 436.17 / 482.64 | 67.84 / 83.68 | 219.08 / 45.97 |
| 2x scale | 614 x 422 | 422.94 / 418.43 | 74.02 / 80.92 | 80.40 / 26.54 |

Two simultaneous jobs also passed the 5% completion-time and peak-memory
regression gates. Each timing measures completion of both jobs:

| Fixture | Baseline median ms | Cropped median ms | Cropped p95 ms | Baseline / cropped peak MiB |
| --- | ---: | ---: | ---: | ---: |
| Medium rectangle | 869.91 | 614.53 | 827.59 | 306.4 / 123.1 |
| Large rectangle | 3019.40 | 2743.90 | 2890.69 | 481.3 / 236.3 |
| Long stroke | 1146.94 | 608.20 | 835.01 | 896.2 / 266.0 |
| 2x scale | 684.11 | 506.80 | 723.99 | 292.6 / 117.1 |

Strict Release builds passed for the benchmark and four related test targets.
Only Smart Erase functional/quality tests, filter rendering and screenshot export
service tests were run. Expanded quality coverage includes larger diagonal holes,
odd pyramid dimensions, thin multispan strokes at 2x scale, negative/fractional
mappings, rotation, partial alpha, disjoint coverage, unchanged native output
mapping, erased-color independence, deterministic cancellation on entering a finer
level, and serial/parallel and exhaustive/bounded scoring equivalence. All passed,
and changed C++ files passed clang-format and whitespace checks.

Export testing initially encountered the same libde265 initialization defect
documented above. A fresh loader trace again located `init_scan_orders` in
`scan.cc:150`, called by HEIF during DLL initialization, before the test reached
main. The staged DLL matched the installed package. Replacing only the generated
test directory's DLL with the existing local Release rebuild allowed the unchanged
export executable to pass. No codec source or installed dependency was changed;
dependency staging can reintroduce the original DLL.

Reproduce the final timing comparison with
`scripts/run-smart-erase-policy-perf.ps1 -Baseline <preserved-executable>` from
this module. It defaults to baseline/default, records executable hashes and runs
the thirteen fixtures. Use `-Jobs 2` and `-Scenarios nonrepeat-medium,nonrepeat-large,nonrepeat-long-pen,double-scale`
for concurrent qualification. `--policy reference` on the current benchmark
reproduces the old reconstruction without needing the preserved executable.
Image comparison remains a separate invocation with `--reference <baseline-images>`.

Local evidence is under `build/smart-erase-adaptive/`: `policy-timings`,
`final-timings`, `concurrent-timings`, saved baseline/final images and
`native-comparisons.png`.
The rejected implementation is preserved there as `experimental.patch`, and its
benchmark as `smart-erase-adaptive-candidate.exe` beside the benchmark binaries.
The policy runner's explicit adaptive choices require that experimental executable;
the shipped benchmark accepts only default/reference/crop policies. Build artifacts
are not committed.

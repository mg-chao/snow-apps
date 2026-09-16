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
masked-object color independence, and unchanged outside pixels.

Run only the related CTest selection:

```powershell
ctest --preset test-windows-msvc-debug -R '^snow-canvas-smart-erase(-quality)?-tests$' --output-on-failure
```

Build `snow-canvas-smart-erase-benchmark` using `windows-msvc-performance` (Release).
Pass an output directory to the executable to save before/after PNG crops. CSV
output reports elapsed reconstruction time and retained original/result bytes.
The benchmark intentionally links the algorithm directly to isolate it from UI
and Rust startup. Timings are single-run development observations, not budgets.

| Fixture | Source | Target | Elapsed ms |
| --- | --- | --- | ---: |
| Repeating texture | 512 × 320 | 96 × 64 rect | 6 |
| Repeating texture | 3840 × 2160 | 192 × 64 rect | 10 |
| Repeating texture | 7680 × 4320 | 192 × 64 rect | 7 |
| Repeating texture | 3840 × 2160 | 384 × 256 rect | 36 |
| Repeating texture | 7680 × 4320 | 2400 px pen span | 161 |
| Nonrepeating waves and gradient | 1024 × 768 | 192 × 96 rect | 561 |

The first five fixtures exercise the verified periodic fast path. The final fixture exercises
general patch synthesis and shows its boundary-quality limitations. These fixtures
do not establish performance or visual parity with Photoshop on arbitrary photos.

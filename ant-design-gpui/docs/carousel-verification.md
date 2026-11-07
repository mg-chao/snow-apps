# Carousel verification

Validated on Windows x64, 2026-10-29, with Rust 1.98.1, the pinned GPUI revision
`2c0bf70b54c018f99419f386d1cf1105dca57661`, and a Ryzen 9 5950X.
The independent reference is Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with
`@ant-design/react-slick` 2.0.0.

## Tests and source provenance

The current worktree passed 118 filtered library tests and four Carousel gallery
tests, strict Clippy with warnings denied, and scoped rustfmt. The library run
includes 48 Carousel tests plus the affected configuration, locale, shared text,
Notification layout, and Image pixel tests. Small behavior-preserving lint
cleanups in those adjacent helpers were necessary for the strict check.
No full test suite was run. Exact commands, test names and hashes are in
[the test record](../fixtures/carousel/tests-windows.json).
The [completion audit](../fixtures/carousel/completion-audit-windows.json) binds the
reports, rechecks both complete snapshots, and verifies the current runtime sources.

Coverage includes finite/infinite navigation, stable keys and retained entities,
empty/single collections, replacement and reordering, initial selection changes,
callback reentrancy, interrupted scroll/fade motion, fade reversal order,
navigation after an elapsed seam transition, drag thresholds and resistance,
keyboard direction, inactive-content exclusion, focus recovery, intrinsic and
adaptive sizing, invalidation, root padding, autoplay pause/resume, visibility,
reduced motion, and cancellation on unmount. Reduced motion keeps timed rotation
and suppresses animated transitions/progress, as requested.

Concurrent Image and Modal edits changed whole-library build inputs during long
verification runs. Final native evidence therefore uses immutable **complete**
source snapshots, including those modules unchanged; no modules were omitted.
S1 contains 2,556 files and supplies the visual and performance evidence. S2
contains 2,558 files and supplies the final accessibility fixture/harness revision.
Both retain creation and post-run hash checks. Their source roots are explicit in
the reports. The Carousel runtime is identical in both snapshots and the worktree.

[S1's attestation](../fixtures/carousel/source-snapshot-validation-windows.json)
confirms 1,731 current component/shared/icon/build inputs match and all 600 native
PNG hashes equal the earlier successful current-worktree captures.
[S2's attestation](../fixtures/carousel/accessibility-source-snapshot-validation-windows.json)
confirms all non-Image Rust sources and the checked resource/configuration/harness
inputs match the current worktree (1,957 files). Concurrent differences and the
exact checked paths are recorded rather than relabeled as tested revisions.

## Visual comparison

The [visual report](../fixtures/carousel/visual-validation-windows.json) contains
600 browser/native pairs and 1,776 component rows. Thirteen scenes cover four
themes, both directions, actual scales 1/1.5/2, every dot placement, orientation
overrides, arrows and finite edges, hover/focus, empty/single content, tokens,
narrow layouts, adaptive height, scrolling, fade, infinite wrapping, and dot
progress. Motion and progress have five deterministic phase samples.

The comparator verifies physical dimensions, case sets, selected slide and dot
associations, and a one-logical-pixel geometry limit for matching cases. Images
and component crops are compared without registration, resampling, or masking.
All 17 positive/negative comparator tests pass; see
[the comparator record](../fixtures/carousel/comparator-tests-windows.json).

| Comparison group | Rows | Maximum crop error / 255 | Maximum raw geometry difference |
| --- | ---: | ---: | ---: |
| Ordinary reference appearance | 1,500 | 1.0961 | 0.359375 px |
| Layered fade with overlapping children | 60 | 5.3378 | 0.359375 px |
| Upstream horizontal RTL fade defect | 180 | 170.7148 | 0.359375 px |
| Upstream RTL adaptive-height defect | 12 | 0.4165 | 60 px |
| Empty internal viewport structure | 24 | 0 (identical pixels) | 160 px |

The exceptions remain in the raw totals and are individually checked:

- GPUI applies inherited opacity to individual primitives; browsers composite a
  child subtree before applying group opacity. Overlapping content in a fading
  slide therefore differs slightly. The pinned GPUI API has no arbitrary native
  subtree compositor; this is the remaining rendering discrepancy.
- The reference places visible horizontal RTL fade slides outside its viewport
  during the captured transitions. Native content remains visible. The comparator
  verifies that actual upstream offscreen placement before classifying this case.
- Upstream RTL adaptive height can use a different source slide's height. Native
  uses the selected slide. The recorded case has a 160 px reference viewport and
  100 px selected content; the difference is preserved explicitly.
- An empty reference Carousel's internal Slick viewport collapses despite the
  explicit 160 px root. Native retains its clip height. Root geometry and every
  pixel match; the internal extent difference is structural.

Normal text/path rasterization residuals remain measured. Five fresh gallery
captures were inspected in wide, narrow, dark RTL, dark compact RTL, and scrolled
bottom layouts; [their hashes and paths](../fixtures/carousel/gallery-validation-windows.json)
are recorded separately.

## Native accessibility

[Native UI Automation](../fixtures/carousel/accessibility-validation-windows.json)
passes nine checks across eight snapshots and six lifecycle stages: real Group
and Button control types, names/descriptions, unique current-dot state,
arrow/dot/content InvokePattern routing, exactly-once change events, inactive
content exclusion, finite edges, locale/RTL updates, retained logical focus,
focused-slide removal, empty/single collections, and repopulation.

Late assistive-technology activation is checked across four ordinary unchanged
draws without forced refresh. This exposed GPUI's cached-view replay omitting
descendant semantics/actions. Carousel bypasses its explicit content cache while
accessibility is active, preserving ordinary rendering caches otherwise.

The fixture stays hidden and inactive. It checks retained GPUI focus ownership
and native focusability, and records raw native focus properties. Foreground
host keyboard focus and screen-reader speech were not exercised; no global
keyboard/mouse input was sent.

## Release performance

The [performance record](../fixtures/carousel/performance-windows.json) contains
41 measurements and seven passing assertions, using the repository's validated
`windows-msvc-performance` Release configuration. Values below measure CPU layout
and scene preparation, in milliseconds:

| Case | Median | p95 |
| --- | ---: | ---: |
| 4 slides, viewport redraw | 0.0684 | 0.0786 |
| 1,000 slides, viewport redraw | 0.0679 | 0.0774 |
| 4 dots enabled | 0.1059 | 0.1177 |
| 20 dots enabled | 0.2247 | 0.2787 |
| 100 dots enabled | 0.8424 | 0.9581 |
| 1,000 dots enabled | 11.9676 | 16.9068 |

Viewport content construction stays bounded from 1 to 1,000 slides. One hundred
warm owner redraws rebuild zero retained content trees. Live-clock checks require
zero settled animation callbacks, one coalesced progress callback across repeated
redraws, timer-only reduced-motion rotation, and drained work after unmount.
All assertions pass. Rendering all dots has a separate linear cost; the 1,000-dot
case is a stress measurement, not a 60 Hz interaction guarantee.

GPU presentation latency/frame pacing and native macOS/Linux behavior were not
verified. The timings are diagnostic for this machine and workload.

## Reproduction

See the [component guide](carousel.md) for native integration and focused Rust
checks, and the [fixture README](../fixtures/carousel/README.md) for original-source
browser capture, native GPU capture, comparison, accessibility and benchmark
commands. Generated images/binaries and complete immutable snapshots remain under
the ignored build tree. The reports retain exact source, tool and executable hashes.

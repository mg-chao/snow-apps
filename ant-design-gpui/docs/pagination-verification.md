# Pagination verification

The reference is Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, using
`@rc-component/pagination` 1.4.0 from the existing locked local reference
dependencies. Verification runs the original implementation from `E:/ant-design`.
No JavaScript component or browser is required by the native library.

[verification.json](../fixtures/pagination/verification.json) records the completed
visual and Release measurement run, including counts, budgets, source and binary
hashes, toolchain, and reference provenance. The fixture packer rejects incomplete
or failing runs. The integration API is documented in [pagination.md](pagination.md).

## Recorded result

The 2026-10-28 run passed all 416 scenarios at each of three scales: 1,248 native
captures and 9,486 surface/text/shaping checks with zero failures. All 1,248
original source PNGs passed the raster integrity audit after the three documented
capture repairs.

| Comparison | Passed |
| --- | ---: |
| Surface geometry, colors, strokes and Ant control radii | 8,574 |
| Total-label PNG line counts | 204 |
| Bidi and wrapped-context shaping against original DOMRanges | 372 |
| Custom Previous/Next PNG line counts | 48 |
| Confirmation-label PNG visibility | 216 |
| Disabled confirmation RGBA | 72 |

The comparison retains unmasked residuals. Mean per-case RGB-channel errors on
the 0–255 scale are 0.13345/0.08588/0.08101 for the fixture crop at
100%/150%/200%; foreground-only means are 35.11/33.40/28.50. The foreground
measure prevents unchanged canvas from hiding glyph, shadow and dash differences;
it is descriptive rather than a substitute for the strict surface gates.

## Implementation and coverage

The controller is a retained GPUI entity; a fresh `RenderOnce` builder composes
native elements around it. Page generation and overflow-safe `u64` arithmetic are
independent of rendering and remain bounded by eleven visible controls. Input and
Select retain their native editing, IME, popup, focus, and accessibility behavior.
Size options are cached and shared with custom renderers; custom requests use a
weak controller connection.

Independent original-browser fixtures provide 222 rendered page models and
seventeen interaction/event traces. Native tests compare those records and also
exercise arithmetic beyond JavaScript's exact-integer domain.

Fractional widths carry from the row's anchored edge. Text totals use native
shaped advances, avoiding wrapping caused by unkerned width estimates. Unicode
visual runs preserve native glyphs and clusters while correcting the pinned
Windows backend's discarded bidi placement. Wrapped lines retain the complete
paragraph's resolved context. The native shaping cache remains immutable.

| Area | Covered behavior |
| --- | --- |
| Page model | Empty/single pages, boundary and middle windows, both ellipses, less items, hidden jumpers, hidden single page, inclusive ranges, `u64::MAX` arithmetic |
| Ownership | Independent controlled current/size, one-time defaults, silent setters, total shrink/restore, owner precedence, size-change then page-change event ordering |
| Editing | Quick Enter/blur/confirmation, page-click blur suppression, simple arrows/Enter, read-only mode, invalid input, native IME composition |
| Composition | Text/custom totals, suppressed items, custom navigation/icons, native Select customization, custom size changer, default/custom confirmation content |
| Configuration | Live themes, sizes, direction, disabled state, motion, three locales and per-field patches; updates through cached parents |
| Lifecycle | Focus transfer, cancellation on blur/disable/hide/viewport change/unmount/controller replacement, rejection of stale requests |
| Motion | Hover/press surfaces, ellipsis transitions, settling, reduced motion, zero idle animation frames |

The gallery implements all thirteen public demos in source order with twenty-eight
independent retained controllers. Longer examples span the gallery grid; narrow
previews retain horizontal scrolling.

## Focused native checks

The final related native checks on Windows x64 passed on 2026-10-28
(Asia/Shanghai), with Rust 1.98.1 and the pinned GPUI revision
`2c0bf70b54c018f99419f386d1cf1105dca57661`:

| Check | Result |
| --- | --- |
| Selected library/model/interaction/locale/config/accessibility tests | 67 passed; 660 unrelated tests filtered out |
| Pagination gallery tests | 4 passed; 60 unrelated tests filtered out |
| Hidden native Windows UI Automation | 7 groups passed |
| Clippy on library/tests/gallery/native fixtures/Pagination benchmark | Passed with `-D warnings` |
| Scoped Rustfmt and whitespace/diff checks | Passed |

The normal manifest is used, with only related tests selected:

```powershell
cargo test --locked --offline --lib -- pagination:: locale::tests config::tests::pagination behavior::tests::semantic_adapter --test-threads=1
cargo test --locked --offline --example gallery pagination_gallery -- --test-threads=1
cargo clippy --locked --offline --lib --tests --example gallery --example pagination_visual --example pagination_accessibility --bench pagination -- -D warnings
scripts/test-pagination-accessibility.ps1
```

The hidden Windows UI Automation checks cover navigation/current-page metadata,
page/previous/next/ellipsis invocation, disabled actions, live availability and
locales, native combobox selection and event ordering, quick/simple/read-only
editors, raw total text in both directions, live range updates, and normal teardown.
Actual total labels preserve logical text rather than exposing formatting controls.
All fixture launches and captures remain hidden; no global keyboard input is sent.

Logs and native evidence are under:

- `build/ant-design-gpui/pagination-validation/`: selected library tests and Clippy.
- `build/ant-design-gpui/pagination-uia/`: result, native tree snapshots, and source/binary hashes.
- `build/ant-design-gpui/pagination-gallery/`: four PNGs, visibility checks, and visual review.

These paths are relative to the parent Snow Apps repository, whose build tree is
ignored by Git.

## Reference comparison

[visual-cases.json](../fixtures/pagination/visual-cases.json) is the shared input
manifest for Chromium and native captures. It covers default, dark, compact and
dark-compact themes; LTR/RTL; all sizes; disabled, hover, held pointer and keyboard
states; simple/read-only, editors, popup options, custom renderers, wireframe,
component tokens, input variants, and Chinese locales. Responsive windows include
480, 575, 576, 577, 800, 991, 992 and 993 logical pixels. Every scenario is captured
at 100%, 150% and 200% scaling.

The strict non-text budget is one logical pixel for geometry, stroke widths and
effective Ant control radii; one RGB byte; and 0.001 alpha rounding. Total and
custom navigation labels also have direct PNG line checks. Independent original
DOMRange measurements verify number/word order, including wrapped paragraph
context. Those shaping diagnostics supplement actual paired PNG review.

The default confirmation button requires actual painted-pixel evidence: Chromium
reports computed styles which differ from its native form painting. Its native
implementation follows measured rest, hover, held pointer, Space, Enter, focus,
disabled and cursor states. Disabled fill and outline blend independently over the
parent at a single physical pixel. At higher resolution, the outline's inner band
blends over the fill while its outer band blends over the parent. Five different
parent colors under both themes establish this translucent paint model. The source
probe is reproducible with
[probe_pagination_go_paint.mjs](../scripts/probe_pagination_go_paint.mjs).

Foreground-text contrast checks prevent a translucent frame from washing out its
label. Their explicit font-antialiasing allowance is separate from the unchanged
surface geometry/color budgets.

Three original browser PNGs contained repeated screenshot tiles. Only those
captures were regenerated in fresh isolated browsers; their DOM/style records
remained exactly equal. Original/replacement hashes and reasons are retained in
[capture-repairs.json](../fixtures/pagination/capture-repairs.json). A separate
whole-manifest raster audit rejects unexpected foreground outside measured
root/popup bounds, allowing normal shadow and inline overflow. It does not relax
the component comparison budgets.

Raw paired PNGs, scene/DOM metrics and per-case comparisons are under
`build/ant-design-gpui/pagination-{reference,native}/`. Checked-in fixtures preserve
compact provenance and measurements; regeneration commands are in the
[fixture README](../fixtures/pagination/README.md).

## Performance

`scripts/run-pagination-perf.ps1` validates the configured
`windows-msvc-performance` preset: Release and benchmarks enabled. It executes
only the Pagination benchmark. Measurements use warmups, allocation counting and
p50/p95/p99 CPU distributions across totals 1,000, one trillion and `u64::MAX`, plus
an open changer and quick editing.

The enforced bounds are zero builder allocations/bytes, at most eleven item
builds per render, at most one Pagination render per measured operation, and zero
idle animation frames. The exact timings and allocations, including GPUI frame
work, are preserved in `verification.json` and
`build/windows-msvc-performance/ant-design-gpui/pagination-results.jsonl`.

All fourteen Release measurements passed. Each measured render built nine items
with one Pagination render per operation; idle animation frames were zero. Builder
samples allocated zero bytes in zero allocations.

| Operation | p50 (microseconds) | p95 | p99 |
| --- | ---: | ---: | ---: |
| Builder, all three totals | 0.1 | 0.2 | 0.2 |
| Warm/forced redraw, range across totals | 143.6–165.1 | 152.5–187.9 | 155.5–193.8 |
| Page change, range across totals | 151.5–155.0 | 163.6–165.5 | 186.4–192.4 |
| Open size changer redraw | 315.0 | 340.1 | 353.8 |
| Quick editing | 261.9 | 282.2 | 305.2 |

These are CPU/allocation observations on this machine with GPUI's deterministic
offscreen text support. They do not establish physical GPU presentation or
input-to-display latency, or a portable timing guarantee.

## Reference details and verification limits

- The pinned reference emits obsolete `mini` for automatic responsive sizing;
  its stylesheet targets `small`. Native automatic sizing preserves that measured
  result, while explicit small works. The source's 576/992 viewport rules are
  preserved independently of the responsive flag.
- In simple mode with a separate quick confirmation button, native confirmation
  commits the adjacent quick draft, matching Enter. This intentionally corrects
  the source's wiring to the simple editor's draft.
- Native current-page semantics, keyboard activation, empty-data availability and
  lifecycle cancellation improve on browser DOM accessibility quirks.
- Glyph antialiasing and pixel placement, SVG rasterization, shadows, and custom
  root dash cadence differ between GPUI and Chromium. Raw foreground and whole
  fixture pixel residuals are retained; passing surfaces do not claim identical
  glyph or shadow pixels. Native form corner appearance is also reviewed manually.
- Animated pixels are not compared frame for frame. Deterministic tests verify
  motion state, easing/scheduling, settling, reduced motion and lifecycle cleanup.
- Validation is Windows x64 with the recorded MSVC/Rust/GPUI toolchain and Edge
  renderer. Native macOS, Linux and ARM64 behavior, other browsers' native form
  painting, and additional locale/font combinations have not been verified.
- GPUI keyboard events and native UIA actions are tested. Visible-window OS
  keyboard injection, physical display latency, and browser OS title-tooltip
  raster appearance have not been verified.
- Windows clamps the gallery's requested 2300-logical-pixel height to 1420 on this
  machine. Gallery PNGs are viewport captures, not a single image of all thirteen
  demos. Focused tests cover every card/anchor; component captures cover individual
  variants, including those below the gallery viewport.

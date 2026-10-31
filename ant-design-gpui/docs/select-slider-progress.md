# Select and Slider verification

Validated on Windows x64 on 2026-10-27 against Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with Select 1.10.1 and Slider 1.1.1.
GPUI remains at `2c0bf70b54c018f99419f386d1cf1105dca57661` without local patches.
The [API guide](select-slider.md) describes usage. The checked-in
[verification record](../fixtures/select-slider/verification.json) preserves
source hashes, measurements, test results and capture summaries. Full captures
and logs are under the ignored `../build/ant-design-gpui/` directory.

## Implementation and coverage

| Requirement | Implementation and evidence |
| --- | --- |
| Native GPUI architecture | Retained typed entities own interaction; `RenderOnce` builders describe presentation. Select uses immutable shared option indexes, the existing native Input editor and popup layer. Slider separates numeric model, interaction state and painting. No browser runtime or new runtime dependency. |
| Select selection and data | Single, multiple, tags, groups, disabled options, clear, loading, label/value identity, controlled value/open/search, remote replacements, selection limits and hide-selected. Model and native interaction tests cover proposals, event ordering and stale editor events. |
| Select search and tokenization | Value/label/named-field search, option- and group-aware custom filters/sorts, remote filtering, separators, custom tokenizer and domain tag factory. Original browser event traces verify separator behavior; created tags participate in sorting and accessibility ordinals. |
| Select editor and focus | Native editor retains IME preedit through unrelated controlled renders. Tests cover composition, paste, Backspace, Enter repeat suppression, Tab traversal, pointer focus and nested popup dismissal. Live hardware IME limits are listed below. |
| Select layout and extension points | Uniform and measured variable-height virtual lists, fixed/responsive/wrapping tags, custom option/label/tag/overflow/popup/empty slots, prefix/suffix, width policies and four placements. Tests cover 100,000 options, distant scroll, narrow responsive editing, compact line boxes and InputGroup joining in both directions. |
| Slider numeric behavior | Fractional and very small steps, marks-only scales, endpoints, ties, finite validation and controlled proposals. Tests execute 323 independently generated upstream numeric/disabled-anchor cases. Snapping never enumerates the step grid. |
| Slider interaction | Single/dual/multiple handles, stable identity through crossing, individual disabling, orientation/reverse/RTL, draggable tracks, editable insertion/deletion and min/max counts. Tests cover pointer capture, keyboard, completion order, cancellation, unmount and 130-pixel drag deletion/restoration. |
| Slider presentation | Marks and custom labels, dots, included intervals/points, independently styled track segments, semantic styles, tokens and lazy tooltips. Dense dot/mark painting is bounded by physical rail resolution; selected dots remain visible. |
| Themes and motion | Four themes, sizes, variants, statuses, custom tokens, semantic refinements and localization. Shared surface transitions and popup entry fade; clipped/reduced-motion loading and settled controls request no unnecessary frames. |
| Gallery | All 27 public Select and 14 public Slider demo IDs are present and navigable. Five tests cover catalogue, layout/anchors, retained data, coordinated selections, async search cancellation and two-way Slider/InputNumber updates. The remote example uses a deterministic local directory. |
| Windows accessibility | 13 real UIA checks pass: combobox, expansion, active descendant, group ordinals, bounded virtual options, distant search, selection, selected state, tag removal, clear, RangeValue, individually disabled handles and keyboard changes. |

The original separator tokenizer reports every resulting selected value. In
multiple mode it can resolve a disabled option by label; tags mode rejects
disabled values. The native implementation preserves this observed distinction.
Direct option selection and tag removal still respect disabling.

## Targeted checks

Rust/Cargo 1.98.1, locked and offline; **257 tests passed**:

| Filter/target | Passed |
| --- | ---: |
| Library `select::` | 31 |
| Library `slider::` | 27 |
| Library `input::` | 97 |
| Library `popup::` | 71 |
| Library `icons::` | 19 |
| Library `locale::` | 5 |
| Library `select_slider_reference_tests` | 2 |
| Gallery `select_slider` | 5 |

Strict Clippy passes for the library, gallery, visual fixture, accessibility
fixture and performance benchmark. Changed Rust files pass rustfmt. No full
repository test suite was run. The license collector recorded 486 notices for
256 external Rust packages and 38 installed vcpkg packages; those vcpkg packages
are included by the shared collector and are not linked by this independent crate.

## Visual comparison

The reference renderer imports the actual checked-out Select, Slider and theme
source. Its locked development dependencies, package integrity values, source
SHA-256 hashes and Edge 156.0.4314.42 version are recorded with the fixtures.
Native captures use a real hidden Windows GPUI window and native DPI changes.
Each renderer owns an isolated capture process/profile.

**784 scenarios at each of 100%, 150% and 200% scaling: 2,352 paired captures.**
They cover default/dark/compact/dark-compact, LTR/RTL, Select modes, three sizes,
four variants, error/warning, disabled/loading/empty/search states, focus,
responsive tags, custom content/tokens, popup placements/widths, and Slider
range/marks/orientation/reverse/disabled/editable/draggable/focus/custom tokens.

**All 5,928 measured surface checks pass** (1,976 at each scale). The gate is at
most one logical pixel of geometry error, RGB error at most 1/255 and alpha error
at most 0.001. It compares resolved fills, relevant borders, dots, rail/tracks,
handle faces and focused outer rings. The comparator rejects mismatched,
duplicate or empty scenario sets and exits unsuccessfully on a failed check.
HTML's inline host baseline offset is normalized to the shared component anchor;
component dimensions and internal/popup offsets remain measured.

Representative screenshots were also inspected together, including open/empty
Selects, compact multiple values, dark RTL, status focus, custom content/tokens,
Slider marks, range and vertical controls. The contact sheet is at
`../build/ant-design-gpui/select-slider-native/review-contact-sheet.png`.
The automated gate does not establish pixel identity for text, text-dependent
tag widths, shadows or animation. Raw image error is recorded separately.

## Release performance

Measured through `scripts/run-select-slider-perf.ps1` with the parent's
`windows-msvc-performance` preset, Release and benchmarks enabled. Machine:
AMD Ryzen 9 5950X (12 cores/24 threads exposed), Windows 11 Pro build 26300.
Each operation has five warmups and 100 samples (snapping: 1,000 samples).
Allocation counts include reallocations and GPUI frame work.

| Operation | p50 ms | p95 ms | p99 ms | Allocations/op |
| --- | ---: | ---: | ---: | ---: |
| Select 100,000-option filter | 2.858 | 3.207 | 3.357 | 16 |
| Select 100,000-option warm open redraw | 0.192 | 0.205 | 0.213 | 614 |
| Select 100,000-option reopen | 0.544 | 0.873 | 0.898 | 1,835 |
| Select 100,000-option scroll | 0.391 | 0.458 | 0.493 | 1,316.5 |
| Select 100,000 responsive selected tags | 0.108 | 0.119 | 0.121 | 354 |
| Slider single-handle redraw | 0.043 | 0.076 | 0.112 | 137 |
| Slider 100-handle redraw | 1.513 | 1.904 | 3.564 | 6,176 |
| Slider drag without tooltip | 0.147 | 0.279 | 0.308 | 420.04 |
| Slider drag with tooltip | 0.240 | 0.310 | 0.358 | 635.01 |
| Slider dense step dots | 0.750 | 0.849 | 1.015 | 138 |
| Slider 100,000 marks | 1.195 | 1.288 | 1.333 | 132 |

Snapping a 100-million-step scale with 10,000 marks measured approximately
0.1 microseconds with zero allocations. Warm Select redraw/scroll work does not
grow with total option count; responsive tags use the same 354 allocations for
1,000, 10,000 and 100,000 selected values. Assertions cap realized options at 24
and measured responsive tag candidates at five in the benchmark viewport.
Option indexes remain shared. Dense paint tests bound work to physical pixels,
and offscreen interaction tests verify no idle frame scheduling.

These are CPU/allocation measurements using GPUI's deterministic test text
system, not GPU presentation or physical input-to-screen latency. Full
distributions and allocated bytes are preserved in the verification JSON and
`../build/windows-msvc-performance/ant-design-gpui/select-slider-results.jsonl`.

## Reproduction

Run the targeted Cargo filters above with `cargo test --locked --offline --lib
<filter>`, and `cargo test --locked --offline --example gallery select_slider`.
Run `scripts/test-select-slider-accessibility.ps1` for native UIA and
`scripts/run-select-slider-perf.ps1` for Release measurements. The UIA fixture
briefly owns foreground focus and sends keys only while its own window is active.

```powershell
node scripts/prepare_select_slider_reference.mjs --upstream E:/ant-design --install true
node scripts/select_slider_visual_cases.mjs ../build/ant-design-gpui/select-slider-reference/all-cases.json --all
cargo build --locked --offline --example select_slider_visual
foreach ($entry in @(@('1', '100'), @('1.5', '150'), @('2', '200'))) {
    $scale = $entry[0]
    $percent = $entry[1]
    $reference = "../build/ant-design-gpui/select-slider-reference/matrix-$percent"
    $native = "../build/ant-design-gpui/select-slider-native/matrix-$percent"
    ./scripts/capture_select_slider_reference.ps1 -Cases ../build/ant-design-gpui/select-slider-reference/all-cases.json -Output $reference -Scale $scale
    ../build/ant-design-gpui/debug/examples/select_slider_visual.exe --cases ../build/ant-design-gpui/select-slider-reference/all-cases.json --output $native --scale $scale
    python scripts/compare_select_slider_visual.py --reference $reference --native $native --output "../build/ant-design-gpui/select-slider-native/matrix-comparison-$percent.json"
}
```

The comparator requires Python with Pillow and NumPy. Browser capture requires
Node 24+ and Edge; preparation installs only locked development reference
dependencies beneath `build/`. Cargo tests use checked-in fixtures and do not
require those tools, a network connection or the original checkout.

## Remaining discrepancies and unverified behavior

- GPUI/Windows and Chromium differ in glyph antialiasing, text baselines and
  resulting tag widths. Non-text surface checks pass; text is not claimed to be
  pixel-identical. Paragraph reading-direction behavior remains constrained by
  the pinned GPUI runtime, although control geometry, navigation and RTL cases
  are covered.
- Shadow tokens match, but blur rasterization differs. Sampled popup-shadow
  pixels at 150% had mean absolute RGB errors of 2.46/255 (default), 0.83/255
  (dark) and 2.01/255 (compact), with maxima of 12/255, 5/255 and 12/255.
- As agreed, popups use the established GPUI entry fade and immediate closure.
  CSS subtree scale/exit animation is not reproduced. Reduced motion and idle
  scheduling are tested; timed browser/native animation parity is not claimed.
- Native Windows UIA and keyboard paths are verified. Composition/preedit,
  UTF-16 native editing and cancellation are exercised deterministically, but a
  live Chinese IME candidate-window session and screen-reader speech output
  were not verified. macOS/Linux native behavior was not verified.
- The gallery adapts the public demos to native controls and local data. It is
  not a pixel-for-pixel recreation of each complete upstream documentation page.
- GPU frame time, physical input latency and performance on other machines
  remain unmeasured. No cross-platform production claim follows from these
  Windows results.

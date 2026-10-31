# Descriptions verification

The component follows the original Ant Design 6.6.7 source in `E:/ant-design`,
revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The native implementation uses
GPUI elements, application-owned content, pure row planning and intrinsic track
constraints. It has no browser or JavaScript runtime dependency.

## Coverage

| Area | Verification |
| --- | --- |
| Horizontal/vertical, bordered/borderless | Original-source browser fixtures and native captures |
| Large/medium/small | Default and inherited sizes; token/CSS assertions |
| Responsive columns and spans | Breakpoint boundaries, partial maps, custom aliases and live resize |
| Packing | Fixed/responsive spans, overflow clamping, filled rows and incomplete final rows |
| Content | Empty/zero/whitespace, multiline, long words, custom native controls and nested Descriptions |
| Styling | Semantic slots, item precedence, scoped resolvers, component tokens and four theme algorithms |
| Direction and scale | LTR/RTL at 100%, 150% and 200%; mixed Latin, CJK, Arabic and Hebrew |
| Borders | Solid, dashed and dotted, including 1/2/3 px strokes and collapsed boundaries |
| Identity and interaction | Stable input identity, value, selection, focus, pointer hitboxes and keyboard actions after reorder/reflow |
| Lifetime and idle work | Scope replacement/unmount, no-op updates, no static animation frames |
| Accessibility | Native description-list terms/definitions, associated names and rich descendants |

The gallery follows all eight public upstream examples and includes a component
token example. The shared capture corpus contains 32 scenes and 768 paired
theme/direction/scale cases. `fixtures/descriptions/reference.json` stores 39
independently measured browser cases for deterministic native assertions.

## Reproduction

Run only the relevant checks:

```powershell
cargo test --locked --offline --lib descriptions
cargo test --locked --offline --lib behavior::text::tests::
cargo test --locked --offline --lib config::tests::
cargo test --locked --offline --example gallery descriptions
cargo test --locked --offline --doc descriptions
cargo clippy --locked --offline --lib --example gallery --example descriptions_visual --example descriptions_accessibility --bench descriptions -- -D warnings
./scripts/test-descriptions-accessibility.ps1
./scripts/run-descriptions-perf.ps1
```

The [fixture guide](../fixtures/descriptions/README.md) documents browser/native
capture and strict comparison commands. Browser packages are developer-only and
use the existing locked reference dependency cache. Full screenshots, metrics,
comparison reports and build manifests stay under the ignored `build/` tree.

## Measurement boundaries

The comparator uses a 1.01 logical-pixel geometry cutoff (a one-pixel target
with 0.01 numeric allowance), flat painted interiors within one channel unit,
and painted coverage outside antialiased edges. It
retains every raw rectangle delta and unaligned image error. It never rescales or
aligns screenshots. Text ranges are excluded from flat-color gates; text fitting
and bidi behavior also have deterministic regressions and manual image review.
Native glyph rasterization is not required to match Chromium pixel for pixel.

Both renderers record actual viewport dimensions. Native windows can quantize a
requested 575 px viewport to 575.333 DIP at 150% scale; the component rounds the
window width to an integral CSS pixel for responsive resolution. Original-browser
metrics confirm `innerWidth = 575`, xs active and sm inactive for that case.

A whitespace-only header can have zero height and produce no GPU quad. Its exact
rectangle is explicitly ungated rather than synthesized; native tests verify its
presence, zero content height, title margin and total component height.

Windows UIA verifies list/item names, named definition groups, editable values
and invoke actions. The activated AccessKit tree additionally verifies definition
roles, matching term/definition sequences and explicit item names. Vertical
visual rows group terms before definitions; the matching names identify pairs.
Keyboard checks dispatch events to the isolated fixture window. Physical OS
keyboard routing, screen-reader speech, macOS and Linux have not been exercised.

Release CPU benchmarks include the complete native subtree redraw under UiRoot.
Their deterministic test text backend does not measure operating-system shaping
or GPU presentation. Supplemental native captures time real DirectWrite draws,
excluding PNG encoding and GPU-completion waits. Large tables are not virtualized.
Resident memory and allocation counts are not instrumented. Timings on the shared
development machine are observations, not fixed latency guarantees.

## Remaining visual differences

Native borderless inline pairs retain the widths their GPUI descendants actually
consume. GPUI snaps absolute lengths and ceilings measured text leaves to device
pixels; Chromium keeps fractional advances. Typical residuals are around 1–2
logical pixels. A shrink-to-content table can therefore be slightly wider than
the browser while correctly keeping its values on one line. Replacing those
native widths with smaller raw advances caused an unwanted extra line and has
been corrected.

Sparse empty/zero-content rows can amplify those rounded track proportions.
Four compact-theme observations at 100% have a maximum cell/content width delta
of 3.34375 px, with position deltas below 2 px; their component heights and slot
presence remain correct. Raw position and width deltas are retained.

At extreme narrow widths, that small difference in the remaining value width
can move an emergency grapheme break, for example `Zhejia`/`ng` versus
`Zheji`/`ang`. This is a recorded discrepancy, not pixel-identical text fitting.
In the `long-text` scene, the bordered Address value takes three lines instead
of four under compact/dark-compact themes at 150%/200%, in both directions.
Those eight component observations are about one 20 px line shorter than the
browser (20.333 px at 150%). At 150%, the browser's usable width is 126.875 px;
the native snapped half-border leaves about 127.333 px. An independent browser
DOM probe measures `Xihu District, Hangzhou,` at 127.03125 px, crossing that
fitting threshold. At 200%, fractional track allocation
can cross the same threshold before final edge snapping. The native text fits
its own allocated box; no artificial fitting slack or font-specific correction
is applied to conceal this renderer difference.

RTL title suffix selection and ellipsis adjacency have both shaped-geometry
regressions and manual review of the final native screenshot against Chromium.

The 3 px patterned borders retain small phase differences from Chromium around
the rounded outer perimeter and shared edge joints. Interior vertical rails can
start/end their dash or dot sequence differently, and overlapping translucent
joint strokes can be darker. Flat color and coverage failures remain visible in
the strict report. They have not been waived by increasing thresholds or aligning
images. Solid border geometry and custom-background corner paint order have
dedicated native regressions.

Open dotted rails wider than 3 px have deterministic geometry tests for complete
end marks at 4/5/8 px and all three scales. Their appearance has not been captured
against the browser; the visual corpus uses authored widths 1/2/3 px.

Nested Descriptions are rendered by both implementations and included in the
outer screenshot's paint gate and manual review. Browser inner-root/view metrics
are recorded, but the native harness does not independently observe every nested
rectangle or semantic slot. No complete nested-geometry parity claim is made.

The three taller diagnostic canvases use a 1000 px viewport so their 200% native
windows fit this Windows display. All scene content fits within that canvas.
Captures retain actual dimensions and fail on a size mismatch; no screenshot is
resized or cropped to conceal a native window-size clamp.

## Recorded results

Recorded on Windows x64 on 2026-10-29. Counts below are per command and include
overlapping filtered tests:

| Check | Result |
| --- | --- |
| `cargo test --lib descriptions` | 57 passed |
| `cargo test --lib behavior::text::tests::` | 21 passed |
| `cargo test --lib config::tests::` | 18 passed |
| `cargo test --example gallery descriptions` | 3 passed |
| `cargo test --doc descriptions` | 1 passed |
| Scoped `rustfmt --check`, Clippy with warnings denied | Passed |
| Reference JavaScript/Python parsing and whitespace checks | Passed |

The complete repository test suite was not run. The final browser fixture update
was followed by another focused Descriptions/configuration test run.

The [Windows accessibility record](../fixtures/descriptions/accessibility-validation-windows.json)
passes all nine stages/checks on binary
`c1fae3a922b3d87026508551322228707f6df3ce483b633705e4df8cdb5adb9d`.
It includes source hashes and the precise scope of native versus targeted-keyboard
validation.

The [Release performance record](../fixtures/descriptions/performance-validation-windows.json)
contains 220 measurements and 72 structural cases. Every structural case has zero
idle renders, zero idle/transition callbacks, and at most one settle frame. All
31 recorded build/measurement input hashes match before and after measurement.
No fixed millisecond threshold was supplied or enforced.

| Real DirectWrite scene, 30 warmed samples | Median CPU draw | p95 CPU draw |
| --- | --- | --- |
| Horizontal: 1 table, 5 items | 0.445 ms | 0.641 ms |
| Long text: 3 tables, 12 items | 1.177 ms | 1.267 ms |
| Bidi: 4 tables, 20 items | 2.188 ms | 3.067 ms |

The deterministic test-text benchmark's single-table redraw medians for
30/300/3000 items are 1.410/13.654/176.014 ms. These observations characterize
scaling of complete redraws; they are not production font or GPU frame timings.

The [visual validation record](../fixtures/descriptions/visual-validation-windows.json)
contains the full 768-case totals, per-scene failures, raw maximum deltas,
reference provenance and artifact hashes. The native capture binary is
`43be4064630ed6815ea438283eb16de7ffece0ab3a80e1f58c40768de140d94b`.
Its build manifest verifies unchanged inputs through compilation; a subsequent
hash audit found no source drift during final capture. Strict visual failures
are retained and explained above rather than reported as universal parity.

Across 1608 component observations, 1410 pass the strict geometry gate, 1444 pass
the paint gate, and 1282 pass both. The 326 observations that fail at least one
gate remain failures in the saved report. Eight compact long-text observations
have the one-line height difference described above; all other root-height
differences are within 0.667 logical pixels. These results establish the coverage
and the remaining differences; they do not claim complete visual parity.

# ColorPicker verification

This document records the checks for the native GPUI ColorPicker and the limits
of each check. The implementation guide is [color-picker.md](color-picker.md).
The reference is Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with
`@rc-component/color-picker` 3.1.1. GPUI is pinned to
`2c0bf70b54c018f99419f386d1cf1105dca57661`.

## Recorded status

The focused library run passed **94 tests, 0 failed** after the final state,
preset-validation, typography and RTL layout changes. The focused gallery run passed
**3 tests, 0 failed** on the same source. Both related InputNumber rendering
regressions listed below also passed.

The Windows native accessibility **background inspection passed all 11 checks**
on 2026-10-29 at 19:07:54 UTC, after rebuilding the fixture from the tested source.
It inspected the actual Windows UI Automation tree.
Its foreground window handle and owner were unchanged, and the fixture never
became the foreground application during that run.

**Final visual capture: complete; strict comparison: not passed.** The R3 run
captured all **720 paired scenes** on 2026-10-29 at 19:33:08 UTC. It retained
**4 geometry failures and 61 surface-sample failures** under the original
thresholds. All border, trigger-content color and selected-preset mark checks
passed. The exact results, residual limits and provenance are recorded below;
capture completion is not a claim that every strict visual gate passed.

**Final Release performance measurements: passed** on 2026-10-29 at 19:25:46 UTC.
The attested offscreen CPU/allocation benchmark completed with stable source
fingerprints and all bounded-work, retained-editor and zero-idle-frame assertions
passing. The measured results and their scope are recorded below.

## Deterministic behavior and state coverage

The library tests combine pure color/model tests with offscreen GPUI rendering
and real pointer, keyboard, popup and retained-input paths. They cover:

| Area | Covered behavior |
| --- | --- |
| Color representation | Floating-point HSB/alpha, retained hue for gray/black, byte RGB/HEX exports, CSS serialization, fallible parsing, comparison against the executed pinned color reference. |
| Input validation | Invalid external percentages rejected before mutation; raw empty gradients canonicalized to clear; unsorted stops stably sorted; preset constructors and raw public fields checked before state mutation. |
| Trigger | Actual click and Enter/Space activation, custom native Button activation, default/custom text, clear display and intrinsic layout in block/row parents. |
| Popup | Escape, outside press, focus return, nested format popup staying inside the open ColorPicker, silent external open changes. |
| SV palette and rails | Pointer capture, clamping outside bounds, hue/alpha precision, keyboard steps, accessibility numeric actions and one completion per gesture. |
| Cleared values | Clear distinguished from transparent color; first SV/hue/alpha edits follow the same fill policy for pointer, keyboard and accessibility actions. |
| Color editors | Retained HEX/RGB/HSB/alpha editor identities, partial HEX drafts, format switching, empty numeric channel mapped to zero. |
| Controlled value | Live proposals, rejected proposals, completion-only acceptance, callback reentry into the same controller and owner echoes during drag. |
| Rollback | Rejected pointer, key and discrete gradient edits restore original stop identities and focus; notify-only owner acceptance preserves the proposal IDs; a newer owner value wins; ownership loss discards pending proposals. |
| Dynamic owner props | Closing, externally replacing a value or excluding the edited mode during a gesture cancels capture and removes hidden stop focus. |
| Lifecycle | Disable, disabled-alpha changes, unmount and scope changes cancel pending gestures without a late completion. |
| Gradients | Insertion, interpolation, crossing, stable stop IDs, minimum interactive stop count, keyboard removal and neighboring focus recovery. |
| Gradient initialization | Gradient-only mode, a supplied one-stop gradient and cleared virtual endpoints; virtual endpoints materialize only on edit. |
| Modes | Duplicate mode constraints removed; reference ordering preserved; cached gradient restored when switching back before editing the single color. |
| Disabled alpha | Alpha controls hidden; every emitted gradient stop becomes opaque while identities, mode and active stop survive normalization. |
| Presets and clear | Selection, clear, keyed group state, supplied gradient values and value equality share the retained color model. |
| Theming and locale | Scoped defaults, token derivation/patches, locale messages, inheritance and independent measured reference geometry. |
| Semantic styles | Root/body/content/description/popup behavior, native palette/rail overrides, standalone styling, authored widths/margins and the correct logical side for custom swatch content in RTL. |
| Fractional DPI | Intrinsic trigger width, exact authored minimum dimensions and focus box correction under physical-pixel rounding. |
| Format label clipping | Three complete HEX/RGB/HSB glyphs inside the actual paint mask across 36 combinations of format, direction, density and 100/150/200% scale; Latin label alignment and directional suffix spacing. |
| Motion | Interruptible hover transitions, no idle frame requests, authored overrides, reduced motion and cancellation on hide/unmount. |

The gallery filter verifies the ColorPicker page/catalog wiring and its complete
set of public and debug examples. Native clipboard, platform IME composition and
screen-reader speech have not been separately exercised end to end by these
ColorPicker-specific offscreen tests; editing uses the shared native Input and
InputNumber controls.

## Reproducible focused commands

Run from the `ant-design-gpui` directory. These filters do not run the full test
suite:

```powershell
cargo check --locked --offline --lib
cargo clippy --locked --offline --lib -- -D warnings
cargo test --locked --offline --lib color_picker -- --test-threads=1
cargo test --locked --offline --example gallery color_picker -- --test-threads=1
cargo test --locked --offline --lib input_number::tests::numeric_handles_animate_in_and_out_and_hidden_windows_cancel_motion -- --exact --test-threads=1
cargo test --locked --offline --lib input_number::tests::cancelled_numeric_hold_does_not_leave_pressed_visuals_after_reenable_or_blur -- --exact --test-threads=1
```

The two InputNumber checks cover its shared rendering path after adding the
internal token override used by ColorPicker. To view the examples:

```powershell
cargo run --locked --offline --example gallery -- --component color-picker
```

The visual comparison harness has focused negative controls:

```powershell
python scripts/test_color_picker_visual_comparator.py
```

## Native Windows accessibility

The safe default profile builds a dedicated native fixture and performs read-only
UIA inspection in the background:

```powershell
./scripts/test-color-picker-accessibility.ps1
```

`-SkipBuild` reuses the already-built fixture and is appropriate only when the
binary is known to match the intended source. The recorded final inspection used
an explicit fresh build followed by that option:

```powershell
cargo build --locked --offline --features test-support --example color_picker_accessibility
./scripts/test-color-picker-accessibility.ps1 -SkipBuild
```

The 11 checks are: trigger Button role, expanded metadata, disabled state, hue
range, alpha range, saturation range, brightness range, editable HEX value,
format ComboBox role, preset Invoke availability and clear Invoke availability.
The range checks inspect numeric bounds; hue also checks its small step. Invoke
availability establishes that the native pattern exists, without invoking it.

The default profile does **not** claim native actuation coverage. An early
interactive probe showed that Windows UIA Invoke activates the target native
window. An isolated `WM_MOUSEACTIVATE` guard on the fixture's own HWND did not
prevent that activation; the guard intercepted zero such messages. Actuation
retries were stopped, and the failure evidence was preserved. The harness does
not call `SetForegroundWindow`, inject global input or change another window.

An explicit `-Interactive` profile remains available for a dedicated interactive
test session. It exercises expansion/collapse, RangeValue/Value edits, presets,
localized names, focus restoration and dynamic gradient removal. That profile
has **not** been reverified after the foreground activation was discovered and
must not be counted as passing native interaction evidence. Offscreen GPUI
interaction tests cover the corresponding component behavior independently.

Machine-local evidence from the safe run:

- [UIA result](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-uia/result.json)
- [Actual native tree](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-uia/inspection.json)
- [Preserved guarded Invoke failure](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-uia/guarded-invoke-foreground-failure.json)

## Independent visual comparison

The browser harness loads the actual pinned Ant Design source from
`E:/ant-design`; it does not reproduce the native implementation. The browser and
native tools consume the same [scenario manifest](../fixtures/color_picker/visual-cases.json).
It includes theme/density variants, both directions, multiple DPI scales,
solid/gradient values, disabled and clear states, presets, format variants,
focused controls, trigger sizes/text, status, semantic overrides, placement and
custom/standalone/split panels.

```powershell
node scripts/prepare_color_picker_reference.mjs
node scripts/capture_color_picker_reference.mjs --output ../build/ant-design-gpui/color-picker-browser-final720
./scripts/capture-color-picker-native.ps1 -Output ../build/ant-design-gpui/color-picker-native-final720-r3
python scripts/compare_color_picker_visual.py --reference ../build/ant-design-gpui/color-picker-browser-final720 --native ../build/ant-design-gpui/color-picker-native-final720-r3 --output ../build/ant-design-gpui/color-picker-native-final720-r3/comparison.json
```

Capture tools accept scene/theme/direction/scale filters. Preparation uses the
checked-in developer dependency lock and validates the upstream revision.
Native capture uses its own hidden HWND and no-activate resizing. The final run
passed **5,041 foreground-owner checkpoints with zero violations**. Each
platform bounds poll is followed by neutral synthetic local pointer input and
settling draws; queued hover notifications settle before GPU readback. It
asserted the actual retained hover state for **840 trigger rows** and the local
pointer for all **720 cases**: 696 neutral and 24 deliberately hovered. The
desktop cursor was not moved. Source/executable/metrics hashes bind each result
to the tested runtime and scenario manifest.

The final matrix contains 30 configurations, four theme/density combinations,
both directions and 100/150/200% scales on a 640×720 logical canvas. The source
browser was Chrome 156.0.8078.48 on Windows; native captures used the GPUI Windows
renderer in the dev profile. These images are visual evidence, not performance
measurements. All **20 comparison negative controls passed**, including missing
or displaced marks, wrong mark/content colors, missing content primitives,
incorrect DPI and excessive geometry/gradient errors.

| Strict check | Checks | Failed | Limit |
| --- | ---: | ---: | --- |
| Layout, drawn handles and trigger-content geometry | 14,112 | 4 | 1 logical pixel |
| Resolved border primitives | 2,136 | 0 | 1 RGB byte; covered top-border pixels are also sampled |
| Actual colored trigger-content primitives | 576 | 0 | 1 RGBA byte |
| Checked-preset stroke silhouette and covered ink | 96 | 0 | 1 logical pixel and 1 RGB byte |
| Surface pixels | 30,696 | 61 | 1 RGB byte for solids; 4 for gradients |

Selected-preset marks use the actual source checked class and pseudo-element
ink color. The complete interior stroke silhouette is compared in both
directions, with a maximum Hausdorff distance of 1 logical pixel; all covered
ink colors matched exactly. Flat preset samples avoid the foreground stroke,
while the entire unmasked cell raster remains in the report. The 24 coincident
`aria-hidden` source handle helpers are counted separately from numbered logical
handles, and all their pixels remain in the raw comparisons. Custom inner
swatch content is matched against its actual native fill primitive, independently
of the surrounding body, so an incorrect RTL content position cannot pass on
body geometry alone.

The strict comparison exits nonzero. Every failure is listed by case in the
[Windows visual record](../fixtures/color_picker/visual-validation-windows.json):

| Residual class | Failures | Maximum difference |
| --- | ---: | --- |
| Caller-rendered custom text trigger width, compact at 150%, both themes/directions | 4 geometry checks | +1.317703 logical pixels |
| Hue rail samples | 31 pixels | 5 RGB bytes |
| Gradient rail samples | 4 pixels | 5 RGB bytes |
| Gradient trigger swatches | 12 pixels | 7 RGB bytes |
| Alpha/checker rail samples | 10 pixels | 14 RGB bytes |
| Translucent checker swatches | 4 pixels | 3 RGB bytes |

The pinned Windows gradient shader adds noise of up to 2/255 to RGB and 3/255
to alpha. Fractional physical-pixel placement and checker edges also affect
these samples. These observations explain rendering limits; they do not convert
failed samples into passes or enlarge the thresholds. The callback width
residual likewise remains above the one-pixel gate. Complete HEX/RGB/HSB labels
were separately checked against their paint masks in 36 offscreen combinations
and visually inspected in native RTL captures at every density/scale.

Unmasked raster statistics are retained separately from strict gates. The table
uses physical-pixel-weighted means on RGB byte values; a pixel is over 16 when
any channel differs by more than 16. A low image-wide average cannot override a
failed geometry, token or surface check.

| Unmasked region | Mean absolute channel error | Pixels over 16 |
| --- | ---: | ---: |
| Full canvas | 0.23146 | 0.24115% |
| Complete component crop with shadow margin | 1.12049 | 1.19680% |
| DOM text/input viewports | 10.89542 | 14.31568% |
| Popup shadow | 0.56800 | 0.16994% |
| Selected preset cells | 4.52131 | 8.04404% |

Browser/native glyph antialiasing and shadow kernels differ. Bottom-border
samples composited with shadows are also retained, with a maximum channel
error of 3 bytes. See the [fixture notes](../fixtures/color_picker/README.md)
for sampling and geometry details. Ordinary gradients preserve supplied
hard-stop boundaries through 128 stops. Above that, painting uses 512 sampled
boundaries, with 64 additional subdivisions for varying alpha, so extreme
gradients are approximated. Performance was measured through 32 stops.

The native source map contains **1,983 file hashes**, including runtime sources,
tests, resources, build configuration and the capture fixture. It was unchanged
across build/capture and is exactly equal to the final R3 Release performance
source map. Each reference and native PNG has its own SHA-256 in the image
manifests. The pinned GPUI checkout had no tracked changes; its Windows shader
hash and the local font-file hashes are included in the compact record.

Machine-local final evidence:

- [Full strict comparison](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/comparison.json)
- [Native source/binary provenance](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/native-provenance.json)
- [Reference image manifest](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-browser-final720/image-manifest.json) and [native image manifest](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/image-manifest.json)
- [Unscaled native preview](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/preview.png), [eight-case unmasked contact sheet](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/contact-sheet.png) and [RTL format-label review](E:/snow-apps-workspace/snow-apps/build/ant-design-gpui/color-picker-native-final720-r3/rtl-format-contact.png)

The review images are display-only crops/thumbnails; original full-canvas images
and all strict comparisons remain untouched. Earlier probe/R2 directories are
preserved diagnostic evidence, and are not the final record.

| Artifact | SHA-256 |
| --- | --- |
| Native executable | `21f1087641163637ab60f5bea0b427b87da3b8a9cc2c44b29f06d561d5eafa08` |
| Native metrics | `f1399f0176f30d0dc94c9f2e5661b50006941be1eb22c57048a508b3d12a09b7` |
| Native provenance | `0f13da43b34670dd7c8c50b8dbab19278c74875765132c12b190a172b815ad9b` |
| Full comparison | `ea875fe66ea741efeafe9e13d566ce10e3d9368c2fc23c33f90d5ef7b2f9bd3c` |
| Browser metrics | `f2ff3d8996f2fc9036d06f63807101d2910eb33e282ed6c495807a2a17cddcc1` |
| Sorted native/Release source hash map | `17fc1cf8bb31e04c357b8dfbf49d8f08f29c08a5b6d7f882479342e7e3914d4b` |
| Comparator | `c3d891b51d9d494735e3109a37cb3448c97a0bc3ed3f37c1a5628c1da8c59b8e` |

## Release performance

Use only the benchmark-enabled `windows-msvc-performance` preset:

```powershell
./scripts/run-color-picker-perf.ps1 -Preset windows-msvc-performance
```

The script checks the preset's CMake cache, builds the ColorPicker benchmark with
Cargo's Release bench profile in a separate target directory, and records source,
toolchain, executable and result hashes. Its measurements cover deterministic
offscreen CPU work and allocations. They do not measure GPU presentation time,
physical input latency or screen-reader latency.

The final R3 run completed at **2026-10-29 19:25:46 UTC** using Rust/Cargo 1.98.1,
`x86_64-pc-windows-msvc`, on an AMD Ryzen 9 5950X with 24 logical processors
visible to the process, under Windows NT 10.0.26300.0 x64. Other debug builds and
native captures were held during measurement. The Release build completed
before running the attested script; each case performs five unrecorded warm-up
iterations.

Times are microseconds per measured operation. Allocation counts are the mean
number of allocation/reallocation calls per operation; allocated bytes are
available in the raw results. Redraws include the offscreen GPUI draw and work
assertions. Drags include deterministic synthetic pointer dispatch, retained
editor synchronization and the resulting draw; they are CPU processing costs,
not physical input-to-display latency.

The 0.0 µs conversion median reflects timer resolution; its p95 is 0.1 µs.
It does not establish zero execution time.

The timed fixture uses an uncontrolled picker with motion disabled. Controlled
consumer callbacks and animation startup are tested functionally, rather than
included in these timings.

| Operation | Samples | p50, µs | p95, µs | Allocations/op |
| --- | ---: | ---: | ---: | ---: |
| HSBA to RGBA | 1,000 | 0.0 | 0.1 | 0 |
| Parse RGBA string | 1,000 | 0.3 | 0.4 | 3 |
| Closed trigger redraw | 200 | 73.2 | 83.5 | 176 |
| Open panel redraw | 200 | 400.0 | 430.4 | 1,429 |
| SV palette drag | 200 | 839.7 | 884.0 | 3,023.52 |
| Hue drag | 200 | 844.3 | 911.6 | 2,999 |
| Alpha drag | 200 | 830.7 | 877.1 | 2,977.5 |
| Opaque gradient redraw, 2 stops | 100 | 412.1 | 432.2 | 1,548 |
| Opaque gradient redraw, 8 stops | 100 | 475.3 | 503.5 | 1,823 |
| Opaque gradient redraw, 32 stops | 100 | 713.2 | 746.5 | 2,898 |
| Translucent gradient redraw, 2 stops | 100 | 476.9 | 515.0 | 1,930 |
| Translucent gradient redraw, 8 stops | 100 | 531.5 | 556.4 | 2,204 |
| Translucent gradient redraw, 32 stops | 100 | 774.6 | 874.6 | 3,279 |
| Translucent gradient stop drag, 32 stops | 200 | 1,567.3 | 1,648.1 | 6,877.5 |
| Warm preset redraw, 24 colors | 100 | 726.7 | 769.4 | 2,825 |
| Warm preset redraw, 240 colors | 100 | 3,509.1 | 3,872.7 | 14,566 |

Every draw asserted stable identities for all six retained channel editors and
zero scheduled frames after settling. Total drawn primitives stayed within 64
for the closed trigger, 256 for the panel, `256 + 4 × stop_count` for gradients
and `256 + 8 × preset_count` for presets. Path vertices stayed within 64 times
the applicable primitive limit. Border-only quads may produce four records with
disjoint clip strips, which the preset bound includes. The closed trigger used
13 primitives; the open panel used 68. The 32-stop translucent scene used 321
primitives and 6,255 path vertices; the 240-preset scene used 1,302 primitives.
The preset fixture lays out all supplied colors, with painting clipped by the
viewport. Handler accumulation is not directly measured by this benchmark.

Machine-local evidence:

- [Exact results, including p99 and allocated bytes](E:/snow-apps-workspace/snow-apps/build/windows-msvc-performance/ant-design-gpui/color-picker-results.jsonl)
- [Source, toolchain, executable and result provenance](E:/snow-apps-workspace/snow-apps/build/windows-msvc-performance/ant-design-gpui/color-picker-provenance.json)

The checked-in [Windows performance fixture](../fixtures/color_picker/performance-windows.json)
retains all 16 measurements, 10 scene counts, exact limits and provenance hashes
from this R3 run, totaling 4,000 measured iterations.

The source manifest contains 1,983 file hashes and was checked before/after the
build and after measurement. The recorded Git HEAD is
`414ce2ac19d5b244924db6f782eab3407cc348ed`; the manifest also binds the working
source and assets rather than relying on HEAD alone. SHA-256 digests:

| Artifact | SHA-256 |
| --- | --- |
| Release executable | `83ff9c45754796e9a0d61c42c2bac2654fa9268df514429cd1f05f9ee82bec8c` |
| Results JSONL | `45f505fdb9a3dcf2cf740b6ee7ce695e7bbaa833ec343cfbb8eba8b5b299f342` |
| Provenance JSON | `43cd69467f91b98efd9da0cc91bc2d7481673bce3867b5b9a9ee58ee57040c42` |
| Sorted source hash map | `17fc1cf8bb31e04c357b8dfbf49d8f08f29c08a5b6d7f882479342e7e3914d4b` |
| Performance CMake cache | `b67758cfc9c342d6d4b276c76a95e397bd49900cfac4c96be8c111a97f336898` |

## Platform and API boundaries

Native visual and UIA validation described here was performed on Windows.
macOS, Linux, ARM64 hardware, alternative screen readers and different GPU/font
stacks have not been validated in this task. UIA inspection is Windows-specific.

The native API deliberately uses typed colors, entities, style refinements,
`ConfigScope` and GPUI's popup host. DOM class names, CSS variables,
`getPopupContainer` and DOM `data-*` forwarding have no direct native API. Native
automation IDs/accessibility labels and semantic style slots serve their native
use cases. The parser rejects CSS variables and unsupported wide-gamut color
functions instead of silently inventing a color.

The disabled-alpha path preserves a gradient's stops rather than flattening the
gradient during normalization. This is an intentional correction to upstream
behavior and is covered by deterministic tests.

Reference source, package integrity metadata, licenses and compact measured
baselines are retained under [fixtures/color_picker](../fixtures/color_picker).
Generated captures, executables and raw measurements remain in the ignored build
tree and are machine-local evidence.

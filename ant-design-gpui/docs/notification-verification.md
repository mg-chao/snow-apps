# Notification verification

The independent reference is Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with
`@rc-component/notification` 2.1.1. The fixture bundles and executes the original
Notification component, hooks, list layout, style generators, and icons. It does
not reconstruct expected markup from GPUI output. Original package licenses and
versions are retained under [fixtures](../fixtures/notification/README.md).

## Scope

The shared scene file defines 25 scenes. Four themes (default, dark, compact,
dark compact), both directions, and three actual device scales produce 600
browser/native image pairs. Cases cover all six placements, narrow windows,
status/custom/hidden icons, optional title/description/close controls, actions,
long words and mixed-direction text, variable-height stacks, hover expansion,
thresholds one/three/five, count limits, offsets, gradients, semantic surfaces,
holder padding/gap, multiline and empty titles, and simultaneous placement groups.

The native renderer owns a hidden window; DPI messages address only its unique
HWND. Captures assert the native scale and original physical dimensions. No
images are resized, aligned, shifted, interpolated, or masked. The comparison
checks notice identities and visibility before comparing logical bounds, complete
images, and unaligned card crops. Text, icon, shadow, and antialiasing residuals
remain in those measurements. Raw captures remain in the ignored build tree.

Deterministic GPUI tests cover defaults, monotonic expiry and progress budgets,
hover pause and opt-out, keyboard pause, updates and placement migration, count
eviction, completion, stale handles, reentrant callbacks/content, disabled and
keyboard/pointer close activation, focus restoration, unmount/window isolation,
stack expansion, holder styling, offscreen/hidden scheduling, interruption, and
delayed list-height shrink. Related Message, shared text, configuration, and root
tests guard the shared host/focus extraction.

The Windows accessibility harness independently queries the real UI Automation
provider, in addition to retaining GPUI diagnostic trees. It checks alert/status
role, live setting, label, description, identifiers, focus preservation, disabled
close state, countdown range values, actual custom-action invocation, actual
close-button invocation, and removal. It sends no global keyboard or mouse input.

The focused Release benchmark uses the repository's validated
`windows-msvc-performance` build tree. It measures 1, 3, 20, and 100 notices in
static, stacked, progress, and reduced-motion configurations, plus keyed update,
open, and destruction. Assertions require zero continuous frames for empty and
settled hosts, one shared callback for animated controllers, unchanged warm icon
parse/raster misses, retained count across keyed updates, and drained work after
destruction. Timing is CPU layout/scene preparation on this machine, not GPU
presentation latency or a universal hardware threshold.

## Recorded results (Windows x64, 2026-10-29)

[Focused tests](../fixtures/notification/tests-windows.json) passed: 34 Notification,
5 gallery, 67 Message, 17 shared text, 5 root, and 18 configuration tests (146 total).
No full test suite was run. Scoped `rustfmt --check` passed.

Strict Clippy is currently blocked by concurrent Descriptions work: one
`double_ended_iterator_last` in `src/descriptions/view.rs` and two `clone_on_copy`
warnings in `examples/gallery/descriptions.rs`. The scoped library, gallery,
native fixtures, and Notification benchmark completed Clippy with those two lint
categories explicitly allowed on the command line. No source suppression or
change to the Descriptions implementation was made to bypass those warnings.

The [visual comparison](../fixtures/notification/visual-validation-windows.json)
contains 600 image pairs and 1,776 notice records. Physical dimensions, notice
identities, and visibility all matched. The maximum absolute card-bound error was
1.0 logical pixel (x 0.3334, y 1.0, width 0.000031, height 0.75). Maximum full-image
mean absolute channel error was 5.0753/255; maximum unaligned notice-crop error was
10.7619/255. These include native text, SVG, shadow, and antialiasing differences;
the geometry result is not a claim of pixel-identical rendering.

The visual, accessibility, and performance records bind the final Notification
implementation to their verified build-source hashes. Concurrent Descriptions
work subsequently changed `src/behavior/text.rs`; the focused tests above were
rerun successfully against that later shared-file revision. The full native image,
UIA, and benchmark records are not relabeled as runs of that later revision.

[Native UI Automation](../fixtures/notification/accessibility-validation-windows.json)
passed all five lifecycle states, numeric countdown inspection, custom-action
invocation, and close invocation. The [gallery review](../fixtures/notification/gallery-validation-windows.json)
records four wide/narrow and light/dark compact RTL captures.

The [Release benchmark](../fixtures/notification/performance-windows.json) passed
all scheduling, cache, and lifetime assertions. On the Ryzen 9 5950X, warm CPU
redraw medians were 0.316 ms for three static notices, 0.290 ms with progress,
and 3.482 ms for a 100-notice collapsed stack. Keyed updates had a 28.8 µs median.
These are diagnostic measurements under the machine's workload; see the record
for sample counts and p95 values, and the limits below for GPU/platform coverage.

## Reproduce

```powershell
node scripts/prepare_notification_reference.mjs --upstream E:/ant-design --dependencies ../build/ant-design-gpui/modal-reference/browser
node scripts/capture_notification_reference.mjs --output ../build/ant-design-gpui/notification-browser
cargo run --locked --offline --example notification_visual -- --output ../build/ant-design-gpui/notification-native
python scripts/compare_notification_visual.py --browser ../build/ant-design-gpui/notification-browser --native ../build/ant-design-gpui/notification-native --output ../build/ant-design-gpui/notification-comparison.json --summary fixtures/notification/visual-validation-windows.json --native-executable ../build/ant-design-gpui/debug/examples/notification_visual.exe
cargo test --locked --offline --lib notification:: -- --test-threads=1
cargo test --locked --offline --example gallery notification_gallery -- --test-threads=1
./scripts/test-notification-accessibility.ps1
./scripts/run-notification-perf.ps1
```

The preparer uses the existing locked dependency cache and does not fetch
packages. Capture filters `--scene`, `--theme`, `--direction`, and `--scale`
select a smaller inspection. Ordinary Rust builds/tests do not require the
reference checkout, Node, or a browser. Evidence records source and binary hashes.

## Renderer and platform limits

The pinned GPUI revision is
`2c0bf70b54c018f99419f386d1cf1105dca57661`. It has no public affine transform for an
arbitrary native element subtree. Collapsed stacks paint the exposed older-card
surfaces at 94%/88% and preserve each card's background, border, and shadow;
expanded cards retain their normal native content. During expansion, native
translation and fading replace the browser's continuous scaling of text/actions.
Unusual custom layouts that place content in a collapsed card's exposed outer
padding can differ from that surface-only representation. The matrix verifies
the ordinary collapsed and expanded endpoints, not every possible custom subtree.

Inherited opacity in this GPUI renderer is applied to primitives rather than an
isolated composited surface. Intermediate fades over contrasting backgrounds can
therefore blend differently from CSS group opacity. Native font rasterization,
inline-icon line-box metrics, shadow kernels, fractional pixel snapping, and focus
indication also have platform
residuals. Static comparison does not claim synchronized intermediate animation
pixels; deterministic tests verify native timing, interruption, and scheduling.

Native checks are Windows x64. macOS/Linux, ARM64, screen-reader speech, and real
GPU frame-time profiling are not verified by this fixture run. Custom application
content retains responsibility for its own labels and interaction semantics.

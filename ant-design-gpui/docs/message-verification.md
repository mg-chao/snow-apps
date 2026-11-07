# Message verification

The reference is Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, including
`@rc-component/notification` 2.1.1. Reference generation executes the original
Message component, notification list, style generators, React hooks, and
cssinjs serializer. Native values do not enter expected browser measurements.

## Coverage

| Area | Verification |
| --- | --- |
| Geometry and themes | Actual browser measurements across default, dark, compact, dark compact, LTR, and RTL |
| Native pixels | Matching original 100%, 150%, and 200% DPI screenshots; no resizing, masking, or origin alignment |
| Content | Five statuses, iconless/custom icons, rich content, normal whitespace, Unicode line wrapping, long words, and mixed-direction text |
| Appearance | Custom root padding, radius and background; semantic wrapper/icon/title styles; component and global token overrides |
| Lists | Independent card centering, different card heights, custom top offset, count eviction, collapsed and hover-expanded stacks |
| Lifecycle | Virtual-clock expiry, hover pause/resume, keyed update deadlines, cancellation, destruction, completion, owner release, window isolation, and immediate post-close pointer/keyboard exclusion |
| Accessibility | Actual hidden-window Windows UIA role, live setting, name, description, ID, focus preservation, rich-action invocation, and removal |
| Gallery | Reference order, native triggers, responsive cards, jump anchors, keyed replacement, loading, sequential completion, stack destruction, and presentation changes |
| Performance | Qualified Release measurements and deterministic idle scheduling/icon-cache assertions |

The shared [scene descriptions](../fixtures/message/visual-cases.json) define ten
scenes. Four themes, both directions, and three device scales give 240 pairs of
browser/native images. The narrow scene uses a 320-pixel logical viewport; the
others use 740 pixels. Every scene is 1000 logical pixels tall. Loading rotation
is paused at its initial pose for static pixel comparisons. Collapsed notices
remain in the identity/visibility records without being treated as visible cards.

The genuine browser layout lets an unbroken token overflow its card in a narrow
window. Native text preserves that behavior and the window clips the overflow;
the comparison does not introduce emergency word breaking to make the example fit.

The comparison includes complete images and unaligned component crops. Native
font rasterization and device-pixel layout can leave small residual differences;
the report retains them rather than masking text or moving either image.

Static screenshots compare settled poses. Ordinary enter/leave and close-reflow
translations, placeholder geometry, timing and opacity are checked with a
deterministic native clock against actual browser transition samples;
this work does not claim a synchronized animated pixel matrix. The pinned GPUI
renderer applies inherited opacity to individual primitives rather than an
isolated composited group. During fades over contrasting or patterned backdrops,
overlapping card backgrounds and child graphics can therefore blend differently
from CSS group opacity. That renderer limitation remains distinct from the
verified timing, position, and opacity model.

Stack reveal uses native fading and vertical motion. The reference also scales
older cards from 94%/88% to 100%; native cards retain their natural size during
that transition. The pinned GPUI revision has no public affine transform for an
arbitrary element subtree, so intermediate stack geometry is a native adaptation.
The collapsed and expanded endpoints remain covered by the settled image matrix;
the motion checks do not establish equality of intermediate card widths or pixels.

## Recorded results

The [Windows comparison](../fixtures/message/visual-validation-windows.json)
records 240 original image pairs and 816 paired notice records. The largest
absolute bounds differences, in logical pixels, are:

| Position or size | Maximum difference |
| --- | ---: |
| Horizontal position | 0.46875 |
| Vertical position | 0.33334 |
| Width | 0.9375 |
| Height | 0.25 |

There is no accumulated card-height drift. The vertical fraction comes from
custom holder padding at 150% DPI; the height fraction comes from the compact
empty status icon's browser inline line box. Across complete, unmasked images,
the maximum mean absolute channel error is 2.0956 on a 0–255 scale, and at most
1.8932% of pixels exceed a luminance-weighted difference of 16. For unaligned
card crops, the maximum mean absolute channel error is 14.6360 on the same
scale. Font rasterization and device-pixel placement differences remain visible
in the retained comparison.

The focused native runs passed 66 Message tests and five Message gallery tests.
The [UIA record](../fixtures/message/accessibility-validation-windows.json)
passed all five hidden-window states, including an actual rich-action invocation
whose callback ran exactly once. Gallery captures checked all ten cards at top,
middle, and bottom scroll positions in the default, dark compact RTL, and narrow
presentations. The recorded visual, UIA, and performance reports include source
and executable hashes.

The Message library, gallery, harnesses, and benchmark pass scoped Clippy with
warnings denied and scoped `rustfmt --check`. A whole-crate formatting check
still reports existing line-ending differences in untouched files; those files
were not rewritten for this component.

## Reproduce

Reference generation reads pinned packages from the existing package store and
explicitly supplied package directories; the generator does not fetch packages.
For this verification, notification 2.1.1 was provisioned from its official npm
archive because the local store only contained 1.2.0. Follow the
[package provisioning instructions](../fixtures/message/README.md) once, then
reuse the extracted cache for offline generation and capture. Ordinary native
Rust builds and fixture tests need no Node runtime or network access.

```powershell
node scripts/generate_message_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --notification-package ../build/ant-design-gpui/message-reference/vendor/notification
node scripts/capture_message_reference.mjs --reference ../build/ant-design-gpui/message-reference --output ../build/ant-design-gpui/message-browser
cargo run --locked --offline --example message_visual -- --output ../build/ant-design-gpui/message-native
python scripts/compare_message_visual.py --browser ../build/ant-design-gpui/message-browser --native ../build/ant-design-gpui/message-native --output ../build/ant-design-gpui/message-comparison.json --summary fixtures/message/visual-validation-windows.json --native-executable ../build/ant-design-gpui/debug/examples/message_visual.exe
```

Select one native scene, theme, direction, and scale for a short inspection:

```powershell
cargo run --locked --offline --example message_visual -- --output ../build/ant-design-gpui/message-preview --scene types --theme default --direction ltr --scale 1
```

The native renderer owns one hidden window. DPI messages address only its
uniquely titled HWND; captures assert the resulting native scale and exact
physical dimensions. Hover expansion uses the window's normal input dispatch,
without sending global input. Raw bundles, browser profiles, screenshots,
native metrics, and diagnostics remain under the ignored build directory.
Checked-in fixtures record source/package hashes and tool provenance so ordinary
Rust tests require no reference checkout, JavaScript runtime, or browser.

The gallery's optional `--capture-scroll-y OFFSET` captures a finite,
nonnegative logical scroll offset after layout. It requires `--capture` and is
clamped by the native scroll area. Top, middle, and bottom captures cover all ten
cards even when Windows limits the height of a requested tall hidden window.

## Accessibility and performance

```powershell
cargo build --locked --offline --example message_accessibility
pwsh -NoProfile -File scripts/test-message-accessibility.ps1 -SkipBuild
pwsh -NoProfile -File scripts/run-message-perf.ps1
```

The UIA fixture activates the normal lazy AccessKit provider by sending
`WM_GETOBJECT` only to its own hidden window. It queries native UIA properties
independently of debug JSON. Five states exercise loading, keyed success,
an explicit polite Status role, destruction, and rich content with a native
action. The fixture invokes that action through UIA InvokePattern and checks
that its real callback runs exactly once. Screen-reader speech and global input
are outside this fixture.

The performance runner requires the repository's `windows-msvc-performance`
tree, verifies Release and enabled benchmarks, and builds beneath that tree.
The benchmark measures cached styles, option construction, opening, destruction,
keyed replacement, and warm draws for 1, 3, 20, and 100 static, stacked, loading, and reduced-motion
notices. It asserts idle frame counts, bounded animation callbacks, drained
work after destruction, and unchanged warm icon parse/raster misses. Timings
describe CPU layout and scene preparation on the validation machine; they are
not GPU frame times or portable CI timing thresholds.

The [recorded Release run](../fixtures/message/performance-windows.json) used an
AMD Ryzen 9 5950X and Rust 1.98.1. Warm CPU redraw times below are median / p95,
in microseconds, for 100 samples per scenario:

| Retained notices | Static | Collapsed stack | Loading | Reduced motion |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 72.7 / 113.4 | 108.2 / 125.6 | 70.0 / 79.5 | 68.0 / 83.8 |
| 3 | 115.5 / 139.5 | 112.7 / 141.7 | 113.9 / 138.7 | 111.2 / 136.3 |
| 20 | 506.0 / 573.5 | 381.3 / 429.6 | 516.2 / 601.7 | 503.8 / 608.9 |
| 100 | 2715.9 / 3653.5 | 1902.0 / 2910.7 | 3240.4 / 4561.6 | 2929.9 / 4039.0 |

Cached style resolution averaged 22.9 ns and option construction 171.2 ns across
200,000-call batches. In 1,000 lifecycle samples, keyed replacement among 100
retained notices measured 4.1 / 6.9 µs, opening 1.4 / 2.3 µs, and destruction
0.7 / 1.1 µs. Those lifecycle timers surround the controller API call; redraw
costs are measured separately. All frame scheduling, coalescing, retained-count,
cache reuse, and drain assertions passed. The runner verifies its source inputs
before and after compilation and measurement.

Native Windows x64 is the validation platform. macOS and Linux require native
verification before reporting equivalent rendering or accessibility results.

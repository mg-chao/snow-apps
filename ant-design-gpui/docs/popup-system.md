# Popup system implementation and verification

Tooltip, Popover and Popconfirm now use one native GPUI popup foundation.
The implementation follows Ant Design 6.6.7 at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e` from `E:/ant-design`.
GPUI remains at `2c0bf70b54c018f99419f386d1cf1105dca57661`.
See [the public guide](popups.md) for application integration.

## Implementation

- `UiRoot` installs the window-local host; custom roots can install `PopupHost`.
  Deferred native elements escape ancestor clipping while preserving event ancestry.
  Weak ownership coordinates nested dismissal, Escape, focus return and unique tooltips.
- The shared lifecycle supports controlled and uncontrolled state, hover/focus/click/
  context-menu triggers, cancelable delays, fresh or retained content, destruction on
  closure, imperative handles and generation-guarded asynchronous completion.
- Placement follows current bounds during prepaint, including cached scrollers,
  content changes and mask changes. There is no layout polling.
- Components provide twelve placements, rounded arrows, overflow flip/shift, scoped
  defaults, typed theme patches, semantic refinements, RTL layout, wireframe and
  custom colors. Popconfirm uses existing small Buttons and localized OK/Cancel
  labels in English, Simplified Chinese and Traditional Chinese.
- Pending confirmation is single flight. Success closes; failure permits retry;
  Cancel remains available. Dismissal does not cancel application work, and stale
  completion cannot affect a later opening or remounted instance.
- Native trigger bindings preserve existing control identities and callbacks.
  Button, Checkbox, Radio, Switch and actionable Tag bind automatically. Custom
  controls can bind their own semantic root with `PopupTrigger`; a compound group
  should expose a primary control or wrap individual items.
- Entry fades use reference timings. As selected by the user, closure immediately
  removes content, input, focus and accessibility registrations. Unique tooltips
  animate the decorative background's position and size over 300 ms by default
  with CSS `ease`; incoming text fades linearly over 200 ms at its final layout.
  Interrupted handoffs start from the last displayed bounds.
- Shadows use the existing SVG renderer with blur-aware sampling, native scene
  leases and a bounded 64-entry / 16 MiB cache. Sharp shadows retain full DPI.
  Timers are canceled or generation guarded, and settled popups request no frames.

## Completed verification

| Area | Evidence |
| --- | --- |
| Placement | 4,560 independent live rc-trigger cases: all twelve placements, both directions, hidden/edge/center arrows, 8/16 px arrow sizes, viewport edges/corners, partially offscreen and narrow anchors, oversized bodies, custom radii and overflow disabled |
| Styles | 600 independently generated upstream browser cases: colors, typography, padding, radii, spacing and rounded-arrow geometry |
| Lifecycle and input | Deterministic offscreen tests cover controlled rejection, trigger combinations, delays, focus, nested dismissal, outside activation, handles, cache ownership, unmount and stale completion |
| Host integration | Cached scene reuse, clipping escape, scroll realignment, mask-only changes, multiwindow isolation and unique scope boundaries |
| Unique motion | Ten independent original CSS interpolation samples, interrupted/replaced owner tests, short-gap geometry retention, outgoing input removal, reduced motion and native frame captures |
| Accessibility | Preserved trigger identity/help, live availability, custom semantic roots, dialog names/descriptions/busy state, safe initial focus and focus return; native Windows checks described below |
| Components and configuration | Empty/custom content, colors, independent scoped defaults, token patches, locale labels, action options, async success/failure/retry/cancel and stale sessions |
| Gallery | All 27 public reference examples in order, plus scrolling/disabled hints, nested confirmation and keyboard-focus examples |
| Resources | Bounded shadow cache, retained scene leases, weak owner cleanup, no raster misses on warm realignment/reopen and zero settled frames |
| Notices | Repository collector completed; isolated npm fixture tools have a lockfile and copied license catalog |

Only affected tests were run. The popup selection contains 70 passing tests,
including the browser style/placement/motion comparisons. Seven targeted gallery
tests pass, including page navigation, responsive cards, controlled closing,
async cancellation, failure/retry and delayed realignment. The affected
`behavior::`, Button, Checkbox, Radio, Switch, Tag, `root::`, `locale::` and
`config::` selections passed after integration.

Validation commands:

```powershell
cargo test --locked --offline --lib popup:: -- --nocapture
cargo test --locked --offline --example gallery popup::tests:: -- --nocapture
cargo test --locked --offline --example gallery popup_gallery -- --nocapture
cargo clippy --locked --offline --lib --tests --example gallery --example popup_visual --example popup_accessibility --bench popup -- -D warnings
./scripts/run-popup-perf.ps1
./scripts/test-popup-accessibility.ps1
```

Targeted rustfmt checks on changed Rust files and `git diff --check` pass.
Unrelated files retain their existing formatting. No full test suite was run.

## Native visual comparison

825 pairs were rendered by a real Windows GPUI GPU window and Edge
156.0.4314.42. Native DPI changes are delivered to the capture window through its
normal Windows DPI handler; images are not resized to simulate scaling.

| Matrix | Pairs | DPI | Maximum bounds error (logical px) | Maximum popup ROI mean error / 255 | Maximum shadow mean error / 255 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Four themes × wireframe × directions × components × placements | 576 | 150% | 1.042 | 5.155 | 0.957 |
| Default theme, all directions/components/placements | 72 | 100% | 0.547 | 5.474 | 0.933 |
| Default theme, all directions/components/placements | 72 | 200% | 0.555 | 5.336 | 0.936 |
| Additional arrow modes, viewport corners, wrapping, semantic styles, radius 16 and custom/preset colors | 105 | 150% | 1.000 | 10.642 | 1.093 |

The ROI excludes the blank window and masks the trigger; shadows are measured
separately. These are measured differences, not a claim of identical pixels.
Native subpixel text rendering differs from headless Edge's grayscale rendering.
The largest extended errors involve long RTL text; the reading-direction
limitation below is not classified as antialiasing.

The comparisons exposed and corrected RTL confirmation action alignment, an
excessive arrow-radius clamp, and the distinction between a Tooltip's visible
corner radius and the global radius used for alignment.

Five native unique-tooltip samples were inspected at actual elapsed times of
2.3, 78.5, 154.4, 230.4 and 356.7 ms. The background moves and grows continuously;
content remains at its final bounds. Each sample starts from a separately settled
source to avoid GPU readback and PNG encoding delaying the next sample.

Local reports are under `../build/ant-design-gpui/`:

- `popup-visual-comparison.json`, `popup-visual-100.json`,
  `popup-visual-200.json`, `popup-visual-extended.json`.
- Native images: `popup-native/`, `popup-native-100/`,
  `popup-native-200/`, `popup-native-extended/`, `popup-native-unique/`.
- Original browser images: `popup-reference/visual/`, `popup-browser-100/`,
  `popup-browser-200/`, `popup-extended-browser/`.

[Fixture instructions](../fixtures/popup/README.md) describe reproduction.
Committed JSON records original source hashes, package versions, browser versions
and tooling provenance. The browser generators do not use Rust layout or style
values as expected results.

## Windows keyboard and accessibility

The final `test-popup-accessibility.ps1` run passed on the interactive desktop.
Evidence: `../build/ant-design-gpui/popup-uia-01aee8a2a3524cf6959bdd2cf5b75c46.json`.

It covers existing Button identity and FullDescription, Tooltip semantics,
controlled Expand/Collapse, exactly-once Enter/Space callbacks, disabled controls,
safe initial Cancel focus, Escape/Cancel focus restoration, async failure/retry/
cancel/stale completion, pending Confirm action removal with its tab stop retained,
immediate accessible subtree removal and normal teardown. The existing Button
native UIA suite also passed after adding the transparent binding.

Dialog description and busy metadata are covered by offscreen accessibility
regressions; the native script does not explicitly assert those two properties.
Windows exposes expanding controls through ExpandCollapse rather than Invoke.

The test includes a minimize/restore activation round trip. Without it, some
startup runs showed the correct focused control inside GPUI while Windows still
reported window-level focus, including for a bare Button. This pinned-platform
startup discrepancy remains; GPUI was not modified.

## Release performance

Measurements use the repository's real `windows-msvc-performance` configuration:
Release with `SNOW_APPS_BUILD_BENCHMARKS=ON`. The script validates that cache and
runs only the popup benchmark. Final local CPU results:

| Operation | Mean | p95 |
| --- | ---: | ---: |
| Placement with flip/shift | 53.8 ns | 100 ns |
| 1,000 bare Buttons, cached / rerender | 5.37 / 27.03 ms | 7.85 / 37.37 ms |
| 1,000 closed Tooltips, cached / rerender | 4.70 / 42.54 ms | 6.57 / 59.39 ms |
| Nested Popovers, cached / rerender | 0.037 / 0.203 ms | 0.055 / 0.294 ms |
| Open confirmation, cached / rerender | 0.047 / 0.240 ms | 0.079 / 0.337 ms |
| Warm force-align | 0.232 ms | 0.352 ms |
| Warm close/reopen pair | 0.298 ms | 0.333 ms |
| Cold shadow size change | 13.223 ms | 13.765 ms |
| Unique-tooltip animation frame, including cold shadows | 1.722 ms | 9.962 ms |

The cold-shadow maximum was 16.192 ms; unique animation maximum was 10.753 ms.
The general shadow sweep retained 33 entries / 7,051,928 bytes with 1,303 hits.
Ten unique handoffs added 37 misses and 163 hits, with 11,031,008 bytes retained.
Bounds and idle assertions passed. These are diagnostic measurements on this
machine, not hardware-independent thresholds or native GPU presentation timings.
The large fully rerendered 1,000-control case exceeds a frame budget; cached views
avoid that work. No claim is made that Tooltips outperform bare Buttons; small
cached-scene differences reflect measurement variation.

## Native differences and unverified environments

- As agreed, entry fades replace subtree zoom and closure is immediate. The
  pinned public GPUI API cannot paint arbitrary exiting content after removing
  its input/focus registrations.
- Unique handoff retargeting uses the configured full duration from sampled
  bounds. CSS can shorten a reversing transition; that browser-specific reversal
  timing is not reproduced. Position and size remain continuous.
- GPUI's pinned text API has no paragraph reading-direction control. RTL layout
  and action order are mirrored, but English punctuation and line alignment in
  RTL paragraphs can differ from the browser. Long-text comparisons document
  this visible limitation.
- For wholly offscreen custom rectangles, placement uses nonnegative intersection
  area instead of rc-trigger's product-of-signed-extents corner case.
- Native macOS and Linux rendering, accessibility and input were not verified.
  Windows startup focus has the activation limitation described above.
- No real GPU frame-presentation latency or hardware-independent performance
  guarantee is established by the CPU benchmark.

The requested implementation and available-platform validation are complete with
these explicitly recorded differences. GPUI and runtime dependencies are unchanged.

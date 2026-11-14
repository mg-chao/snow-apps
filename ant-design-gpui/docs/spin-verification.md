# Spin verification

Spin is checked against the actual Ant Design 6.6.7 checkout at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The browser generator mounts its
original Spin, Indicator, Looper, Progress and usePercent implementations with
the original styles. Rust component values do not enter the browser fixtures.

## Visual coverage

The shared scenes cover three sizes, standalone descriptions, hidden loading,
progress from negative values through values above 100, nested content, custom
indicators, component tokens, semantic styles, fullscreen loading and different
parent backgrounds. Four themes, both directions, and native 100%, 150% and
200% DPI produce **192 image pairs and 672 measured Spin rows**.

The first seven scenes supply 168 image pairs on the reference's standard
container surfaces. The final 24 pairs deliberately expose the different
compositing behavior on contrasting and patterned parent surfaces, described
below. Both groups remain in the raw, complete pixel measurements.

Browser animations are paused at their exact initial pose: 45-degree rotation
and 0.3 dot opacity. An additional 56 browser captures sample seven animation
times in every theme and direction. Native virtual-clock tests independently
check the same dot motion, progress transitions, delay cancellation and automatic
progress. The phase captures are diagnostics rather than unsynchronized native
animation screenshot comparisons.

The native harness sends DPI changes only to its own uniquely titled hidden
window, verifies the resulting platform scale, and checks exact physical image
dimensions. Fullscreen captures include the whole client viewport. All images
are compared without resizing, interpolation, font masking or origin alignment.
Additional component crops use the union of the actual browser/native bounds;
they retain placement errors.

The checked-in [visual summary](../fixtures/spin/visual-validation-windows.json)
records source hashes, browser provenance, image hashes, physical dimensions,
geometry deltas and pixel errors. Raw PNGs and detailed results remain under
the ignored build tree. Font rasterization and the native framework's device
pixel layout policy account for residual text and fractional-size differences.
The Windows path renderer uses 4× MSAA on this machine, while browser edge
coverage differs. Enlarged tiny circles show that coverage difference even
though native circle paths retain fractional positions and sizes. Native text
also omits the reference's 5 px container-colored description shadow; its main
visible effect is the light-theme fullscreen description. The browser captures
retain that shadow, so its difference remains in the comparison.
The gallery was also checked at wide and narrow widths, including all nine
public examples and the dark compact RTL presentation.

The final gallery binary was rebuilt after the circle-path painting change and
its default presentation recaptured at 1280×2000 physical pixels at 100% DPI
(`spin-gallery-default.png`). All nine public examples fit without clipping.
Additional dark compact RTL and 640×720 logical-pixel narrow captures verified
wrapping and layout; a tall 640-logical-pixel-wide capture shows all nine single-column
cards. These are native hidden-window captures without image resampling, kept
under `build/ant-design-gpui/`. All five focused `spin_gallery` tests passed.

The final binary was rebuilt after the scheduling fix, and all 192 pairs were
captured again from that executable. Its hash and modification time accompany
the source hashes in the visual summary. Measured results are:

| Capture group | Image pairs / rows | Maximum bounds error, logical px | Maximum full-image mean channel error, 0–255 |
| --- | ---: | ---: | ---: |
| Reference container surfaces | 168 / 624 | 0.578125 | 0.153510 |
| Contrasting parent backgrounds | 24 / 48 | 0 | 5.398403 |

The corresponding maximum unaligned component-crop mean errors are 9.292929
and 71.914435. The second group deliberately records the compositing difference
below; it is not classified as matching the reference's colors.

## Nested opacity on different parent backgrounds

GPUI applies ancestor opacity to individual primitives. The native component
uses one container-colored wash to preserve live child rendering without
allocating content snapshots.
This matches the reference when the wrapped content and its parent share the
container surface. It does not reproduce CSS isolated group opacity when an
opaque content panel sits over a contrasting or patterned parent.

For an opaque panel color `B` over a parent color `P`, the reference's loading
panel interior becomes `0.5 B + 0.5 P`; the native panel interior remains `B`.
For example, the light-theme uniform-parent probe is `(161, 183, 204)` in the
browser and `(255, 255, 255)` natively. The two `backdrops` rows record that
difference explicitly, including stripes, without masking it from the report.

## Reproduce the captures

Use the existing pinned package cache and extracted upstream util package; the
generator downloads and installs nothing:

```powershell
node scripts/generate_spin_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_spin_reference.mjs --reference ../build/ant-design-gpui/spin-reference --output ../build/ant-design-gpui/spin-browser
cargo run --locked --offline --example spin_visual -- --output ../build/ant-design-gpui/spin-native
python scripts/compare_spin_visual.py --browser ../build/ant-design-gpui/spin-browser --native ../build/ant-design-gpui/spin-native --output ../build/ant-design-gpui/spin-comparison.json --summary fixtures/spin/visual-validation-windows.json --native-executable ../build/ant-design-gpui/debug/examples/spin_visual.exe
```

A quick native preview can select a scene, theme, direction and device scale:

```powershell
cargo run --locked --offline --example spin_visual -- --output ../build/ant-design-gpui/spin-preview --scene basic --theme default --direction ltr --scale 2
```

## Performance and idle work

The [performance record](../fixtures/spin/performance-windows.json) contains
24 measurements on an AMD Ryzen 9 5950X with Rust 1.98.1. It records builder and
cached/theme token lookup costs, plus warm window draws for 1, 20 and 100 hidden,
animated, determinate, automatic-progress, nested, reduced-motion and static custom indicators.
These are CPU layout, accessibility and scene-preparation measurements rather
than GPU frame times. The raw result file is retained under the performance
build tree.

Determinate mode measures settled cached rings. Automatic mode advances virtual
time by 16 ms per draw, including 200 ms target updates and unfinished progress
interpolation, so it exercises active arc rebuilding rather than only cache hits.

The final quiet run measured these CPU window draws:

| Workload | 1 Spin median, µs | 20 Spinners median, ms | 100 Spinners median / p95, ms |
| --- | ---: | ---: | ---: |
| Animated dots | 82.1 | 0.668 | 3.770 / 4.728 |
| Settled progress | 39.4 | 0.345 | 2.071 / 2.564 |
| Active automatic progress | 101.3 | 0.959 | 6.251 / 9.922 |
| Nested loading with text | 139.7 | 1.549 | 9.463 / 12.033 |

Cached styling cost 33.3 ns, theme lookup 50.7 ns, and builder construction
227.0 ns per iteration. Removing redundant 200 ms auto timers reduced the
100-indicator automatic-progress p95 from 277.308 ms to 9.922 ms, a measured
27.95× reduction. The record preserves the before/after state hashes and the
deterministic notification regression that verifies the mechanism.

The final quiet log and a separate run under external MSVC/Rust compiler
contention are both retained. Their results are recorded separately rather
than treating busy-machine timings as the quiet measurements.

The benchmark also asserts that rerenders do not accumulate frame callbacks;
hidden, settled determinate, reduced-motion and static custom indicators request
zero animation callbacks; and stopping loading drains continuous work. Cached
circle and ring geometry avoids rebuilding stable shapes on each draw.

Run the focused Release benchmark only through the qualified performance tree:

```powershell
pwsh -NoProfile -File scripts/run-spin-perf.ps1
```

The runner requires `windows-msvc-performance`, verifies Release configuration
and enabled benchmarks, and uses that preset's target directory. It refuses an
unqualified build tree. Results characterize this machine and are not portable
timing thresholds. Deterministic scheduling assertions remain the stable checks.

Run the related correctness tests without a full repository suite:

```powershell
cargo test --locked --offline --lib spin::
cargo test --locked --offline --example gallery spin_gallery
```

The final focused checks passed **29 Spin tests**, **17 configuration-related
tests**, and **five gallery tests**. Formatting, reference-script syntax checks,
and Clippy with warnings denied cover the implementation and its native tools.
The component tests include the regression for redundant automatic-progress
timers: continuous animation owns the frame wakeup, while static or reduced-motion
automatic indicators retain their coarse timer. Both modes use the same elapsed-time
progress model.

## Native accessibility and verification limits

The [Windows accessibility record](../fixtures/spin/accessibility-validation-windows.json)
contains seven actual hidden-window UI Automation checks: polite live regions,
busy state, labels/descriptions/automation IDs, progress roles and numeric ranges,
indeterminate progress, retained children, stopped loading, and deferred fullscreen
content. The harness activates only its own window's normal AccessKit provider;
it does not send global keyboard or pointer input.

```powershell
pwsh -NoProfile -File scripts/test-spin-accessibility.ps1
```

Native builds, GPU captures, and accessibility were verified on **Windows x64**.
Windows ARM64, macOS, and Linux native builds, rendering, and accessibility have
not been verified. Performance numbers measure CPU window preparation; GPU frame
times and end-to-end application frame rates were not measured. Timed native
animation is verified through deterministic scene/state tests rather than
synchronized native/browser video captures.

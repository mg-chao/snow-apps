# Progress verification

Verified on Windows x64 on 2026-10-28. All 408 image pairs (2,712 component rows),
15 focused library tests, three gallery tests, twelve native accessibility states,
and six native gallery captures passed. Strict Clippy and targeted formatting
checks passed. No full test suite was run.

The reference is the actual Ant Design 6.6.7 checkout at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e` with `@rc-component/progress` 1.0.3.
The browser harness imports the original component and styles. Native component
values and screenshots do not generate the browser's expected results.

## Visual comparison

The matrix has 17 scenes and 113 inputs across default, dark, compact, and
dark-compact themes, LTR/RTL, and 100%, 150%, and 200% DPI. It covers controlled
values and statuses, success overlays, line/circle/dashboard geometry, caps,
gaps, steps and rounding, solid and multi-stop translucent gradients, semantic
styles, component tokens, custom native content, relative widths, and label
placement. Gallery coverage includes all sixteen public reference examples plus
a component-token example.

Native captures use a dedicated hidden window. DPI changes target only its own
HWND. Browser captures use a dedicated headless process and profile. Neither
harness changes global display settings or accesses a user's browser session.

Images are never resized or aligned. The paint gates use reference-derived masks
and exclude the reference label box with a two-logical-pixel text margin. Shape
coverage allows one logical pixel, rounded to device pixels. Interior color
samples exclude antialiased edges; full unmasked row error is also recorded.

| Measure | Gate | Observed worst case |
| --- | --- | --- |
| Root position/size difference | 1 logical pixel (0.01 numerical allowance) | 0.703125 logical pixels |
| Painted coverage outside allowed edge tolerance | 0 pixels | 0 pixels |
| Solid interior channel error | 1/255 | At most 1/255 |
| Gradient interior mean channel error | 3/255 | 1.8442/255 |
| Gradient interior p99 channel error | 8/255 | 5/255 |

The mean of unmasked row errors is 0.3893/255; the largest row mean is 2.5408/255.
These raw values include native/browser glyph and edge rasterization differences.
134 tiny, transparent, or otherwise non-flat rows have no qualifying interior
samples; their geometry and coverage gates still run. Text is visually reviewed,
not claimed to be pixel-identical.

The [complete visual report](../fixtures/progress/visual-validation-windows.json)
records every row, image hash, native source/binary hash, browser version,
reference dependency lock, bundled source hashes, and thresholds. Regeneration
commands are in the [fixture guide](../fixtures/progress/README.md).

## Behavior, motion, and accessibility

The [focused test report](../fixtures/progress/test-validation-windows.json)
records commands and source hashes. Tests exercise bounded values and statuses,
logical gaps, step rounding, gradient interpolation, scope invalidation, custom
content measurement, cache reuse, interrupted transitions, reduced motion, and
hidden/clipped/unmounted scheduling. Gallery tests cover reference ordering,
responsive cards, navigation anchors, and interactive control bounds.

Windows UI Automation passes twelve states, including unmounting,
custom content and tiny circles. The hidden test verifies actual native role,
label, description, automation ID, numeric range, read-only state and lack of a
tab stop. See the [UIA evidence](../fixtures/progress/accessibility-validation-windows.json).
The [six native gallery captures](../fixtures/progress/gallery-validation-windows.json)
include narrow layout, dark/compact/RTL, and the full page at several scroll
positions. They run with the default Windows stack; no linker stack override is
needed.

Browser CSS transitions are sampled at deterministic paused times. The fixture
contains line widths, circular dash offsets, step colors and active-sweep matrices.
Native virtual-clock tests compare line widths within 0.51 logical pixels,
circular dash offsets within 0.002 viewbox units, and active matrix/opacity values
within 0.00001. These are deterministic state/geometry comparisons against actual
browser samples, not synchronized native animation screenshots.

## Performance

Release CPU measurements used `windows-msvc-performance`, Rust 1.98.1, and an
AMD Ryzen 9 5950X. The benchmark measures 1, 20 and 100 controls in eight modes:
30 cold-construction samples, 100 warm-draw samples, and 100 value-update samples
per combination. Updates alternate 45% and 65% with motion disabled. Timings
include layout, painting, and process allocation counters; they are offscreen CPU
measurements, not end-to-end GPU frame latency.

| Mode, 100 controls | Cold median | Warm median | Update median | Update p95 |
| --- | ---: | ---: | ---: | ---: |
| Line | 6.003 ms | 2.317 ms | 5.463 ms | 7.103 ms |
| Circle | 11.002 ms | 2.882 ms | 8.431 ms | 11.814 ms |
| Dashboard | 10.430 ms | 2.395 ms | 7.277 ms | 10.331 ms |
| Gradient line | 6.881 ms | 2.370 ms | 6.247 ms | 9.514 ms |
| Gradient circle | 39.775 ms | 8.596 ms | 33.523 ms | 40.942 ms |
| Line steps (1,000, clipped) | 5.672 ms | 1.493 ms | 3.090 ms | 4.401 ms |
| Circle steps (100) | 29.288 ms | 2.960 ms | 6.439 ms | 8.572 ms |
| Custom inner content | 7.383 ms | 2.938 ms | 6.447 ms | 8.086 ms |

Single-control warm medians range from 37.1 to 92.0 microseconds. All 24 resting
frame-scheduling assertions pass. The builder occupies 376 inline bytes on this
target; optional style refinements allocate only when used. Geometry and brush
caches, compound circular-step masks, and visible-range line-step realization
avoid rebuilding or drawing work that can be reused or clipped.

The [72-measurement report](../fixtures/progress/performance-windows.json) includes
p95 timings, allocation counts/bytes, source and executable hashes. No absolute
frame-time budget was specified. Updating 100 gradient circles is the remaining
expensive stress case and exceeds a 16.7 ms CPU budget on this machine; these
measurements do not establish universal 60 fps behavior.

## Native differences and verification limits

- The API uses native elements, semantic style slots, logical direction, and
  scoped configuration. Deprecated browser aliases and DOM/CSS class names are
  not duplicated. Native accessible values retain fractional precision instead
  of truncating to an integer.
- Reference quirks are preserved: explicit status controls automatic success;
  circular gradients use butt caps and composite over their linear underpaint;
  circular gradient steps follow the reference's transparent active segments;
  explicit semantic opacity can reveal a minimum round-cap mark at zero percent.
- Circular gradients use bounded angular approximation. Extremely dense circular
  steps below pixel resolution aggregate coverage over the complete ring.
- Native text shaping and edge antialiasing differ from Chromium. The visual
  results apply to the recorded inputs and tolerances, not every possible style.
- Percentage, solid-color, step-color, and active-sweep motion are tested.
  Simultaneous gap, radius, stroke-width, cap, and style reconfiguration has no
  exhaustive frame-by-frame comparison; SVG dash transitions during simultaneous
  radius changes may differ.
- Native macOS, Linux, Windows ARM64, screen-reader speech, and GPU/presentation
  latency were not verified.

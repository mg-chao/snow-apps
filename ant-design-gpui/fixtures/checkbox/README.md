# Checkbox browser reference fixtures

`reference.json` records 576 independently computed native browser states from
Ant Design **6.6.7**, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
The matrix covers default, dark, compact and dark compact themes; rest, hover
and keyboard focus; every checked/indeterminate/disabled combination; empty,
single-line and wrapped labels; and both LTR and RTL layout.

The generator executes the original Checkbox style TypeScript, the original
`@ant-design/cssinjs-utils` CSS calculator and the original
`@ant-design/cssinjs` serializer. Only React style-hook registration and theme
plumbing are adapted. Concrete aliases come from the independently executed
[theme fixtures](../theme/README.md). `controlInteractiveSize` and `lineWidth`
retain `px` units for the source's direct CSS `calc()` interpolation, as they
do in the real CSS-variable mode. No Rust declarations supply browser styles.

Static DOM matches the source Checkbox input, wrapper, icon and label classes.
Microsoft Edge computes the CSS and layout. Attribute substitution activates
the source's hover and focus-visible selectors without changing their
specificity, declarations, media queries or disabled guards. Decorative motion
is disabled during capture. Generated HTML is an inspection aid; component
runtime behavior is covered by native offscreen tests.

The fixture includes exact renderer/package versions, upstream source hashes,
generator/capture hashes and the theme-fixture hash. It was captured using
Microsoft Edge 156.0.4314.42 on Windows with device scale 1. Rust tests compare
colors, dimensions, spacing, focus outlines and mark appearance. Tick polygons
are also compared against independent evaluation of the recorded browser
pseudo-element bounds, transform matrix and transform origin, allowing only
Chromium's 1/64px layout quantization. The tick stays LTR inside RTL labels.

The full flag matrix preserves the source's subtle combined-state cascade:
checked + indeterminate is white at rest, while the more specific wrapper
checked-hover rule uses the hover primary background and transparent border.
Disabled controls retain the neutral border and suppress all hover changes.
Empty labels retain one text line's wrapper height, and wrapped labels center
the icon across all text lines.

To regenerate with existing local packages:

```powershell
./scripts/capture_checkbox_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
```

`-EdgePath` selects an installed Edge; `-OutputPath` writes an alternate fixture.
No packages are installed, no network is required and the capture uses fresh
temporary browser profiles. Generated HTML, raw DOM and browser profiles stay
under ignored `build/ant-design-gpui/checkbox-reference/`.

To generate only the four inspection pages:

```powershell
node scripts/generate_checkbox_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
```

Open a generated page with `?state=hover` or `?state=focus`. Ordinary Cargo tests
consume only the checked-in fixture and require no Node.js, browser, pnpm store
or upstream checkout. The Ant Design MIT license is retained in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

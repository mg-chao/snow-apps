# ColorPicker reference fixtures

`visual-cases.json` is shared by the independent browser and native capture tools.
Four theme/density combinations, two directions and three DPI scales produce
720 scenes across 30 configurations. The source renderer imports the actual Ant Design 6.6.7 checkout at
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e` and rc-color-picker 3.1.1. It uses the
existing checked-in `fixtures/select/browser-package-lock.json`; dependencies and
captures remain in the ignored parent `build/ant-design-gpui/` tree.

Run `node scripts/prepare_color_picker_reference.mjs`, followed by
`node scripts/capture_color_picker_reference.mjs`. Use `--scene`, `--theme`,
`--direction`, and `--scale` to capture a focused subset. `--dependencies` points
preparation at an existing installation of the exact checked-in lock; use
`--install true` to provision that developer-only directory if necessary.

The harness records browser engine details, source SHA-256 hashes, the generated
bundle hash and complete locked package integrity metadata. Native and browser
images use a 640×720 logical canvas with a common (48,48) component anchor.
Browser glyph antialiasing and shadow blur are assessed separately from surface
geometry, flat colors and sampled gradients.

The extended cases include status and hover borders, focused editors and hue
handles, arrow-free bottom-right placement, gradient-only/cleared/one-stop
values, a custom trigger, and native standalone/split panel counterparts.
`pack_color_picker_reference.mjs` saves the 240 measured 100% baselines in
`style-reference.json` for offline reference tests. Original loaded dependency
licenses and versions are in `licenses/` and `reference-packages.json`.

Run `python scripts/test_color_picker_visual_comparator.py` to exercise the
comparison gate's negative controls, including missing scenes/surfaces, DPI
mismatches, excess geometry error and gradient color errors. The 20 controls
also reject a missing, displaced or incorrectly colored selected-preset mark
and independently detect an offset or missing trigger-content fill.

The gate checks actual thumb centers and drawn sizes. The original slider adds
an `aria-hidden` active-handle helper over the numbered logical handle when it
is focused. Coincident helper bounds are recorded separately; numbered logical
handles determine the control count, and all source pixels remain in raster
comparisons. A helper with distinct bounds is rejected for investigation.

Checked presets use the source's checked class and actual pseudo-element ink
color. Their complete interior stroke silhouettes must agree within one logical
pixel and fully covered ink must match within one RGB byte. Flat preset samples
avoid that foreground stroke; the raw, unmasked cell raster is also retained.

Colored trigger-content elements are measured independently of the surrounding
swatch. Their actual native solid-fill primitives must agree in position/size
within one logical pixel and in color within one RGBA byte. This catches custom
content aligned to the wrong logical side while leaving the body unchanged.

Resolved native border primitives must match the browser's computed border
color within one RGB byte. Fully covered top-border pixels have the same gate;
bottom-border pixels composited with Button or popup shadows are reported as
raster residuals. Unmasked component crops, DOM text/input viewports and popup
shadow regions retain their raw image errors. No image-wide average substitutes
for failed geometry, border-token or surface checks.

`scripts/capture-color-picker-native.ps1` binds native captures to SHA-256 hashes
of the source, shared cases, compiler, executable and metrics. It checks source
stability before and after building/capturing. Its hidden native window resizes
only its owned HWND with `SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE` and records
foreground-PID guards at every DPI/frame/capture checkpoint. Performance uses
`scripts/run-color-picker-perf.ps1`, the Release performance preset, and a
separate executable/source provenance record.

`visual-validation-windows.json` and `performance-windows.json` record this
machine's measured results, environment and artifact hashes. The visual record
includes every failed strict gate; its capture-success status does not imply
that every pixel or geometry threshold passed. Full images, comparison details,
source manifests and display-only review crops remain in the ignored build tree.

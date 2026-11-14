# Form reference evidence

The browser harness executes the unmodified Form from Ant Design **6.6.7**,
revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, and
`@rc-component/form` **1.8.6**. Its CSS-in-JS, controls, locale, and four original
theme algorithms run together. JavaScript is a developer-only reference tool;
the native library uses GPUI. Original notices for every loaded package are in
`licenses/`, and `reference-packages.json` records their versions and hashes.
The existing locked local browser toolchain is reused without installing packages.

Final execution evidence is indexed in [the verification report](../../docs/form-verification.md).
`test-validation-windows.json` records the 15 focused library filters and hashes
their logs. `accessibility-validation-windows.json` records six native UIA
profiles, four settled frames per profile and the unsupported Windows invalid
property. `performance-validation-windows.json` retains optimized artifact
provenance, CPU/allocation distributions and structural assertions. These compact
records refer to raw machine artifacts under the ignored build tree; they do not
establish native execution on macOS or Linux.
`gallery-validation-windows.json` binds eight focused gallery tests and two real
hidden captures to their source, executable, screenshot and foreground-trace proof.

Regenerate the shared manifest, original bundle, screenshots, and behavior traces:

```powershell
node scripts/form_visual_cases.mjs
node scripts/prepare_form_reference.mjs --upstream E:/ant-design
node scripts/capture_form_reference.mjs --fixtures true
node scripts/capture_form_behavior.mjs
node scripts/capture_form_inheritance.mjs
python scripts/audit_form_reference.py --browser ../build/ant-design-gpui/form-browser --output ../build/ant-design-gpui/form-browser/audit.json
python scripts/test_form_visual_comparator.py
node scripts/probe_form_margin_reference.mjs
node scripts/pack_form_reference_validation.mjs
```

The default browser is an isolated headless Chrome instance with its own profile.
Each DPI starts a browser with its actual `--force-device-scale-factor`, and the
capture verifies the launch DPR before applying a matching viewport override.
CDP scale emulation alone preserves 100% CSS border snapping; it cannot supply
the 150% native baseline. `dpi-reference.json` independently demonstrates the
difference with a minimal painted input.
Use `--browser <absolute-executable>` to select another Chromium executable.
All raw screenshots and large complete metric records remain under ignored
`../build/ant-design-gpui/`. Source hashes, dependency-lock hash, actual renderer,
and screenshot hashes are retained. Captures freeze animation at its initial
phase to stabilize the validating feedback glyph.

`visual-cases.json` defines **19 application compositions**, captured under
default, dark, compact, and dark-compact algorithms; LTR and RTL; and device
scales 1, 1.5, and 2: **456 actual browser captures**. Scenarios cover horizontal,
vertical, inline, mixed Item layouts, sizes, required/optional/hidden marks,
label tooltip, validation/help/extra/feedback, wrapping, absent versus null
labels, narrow viewport and both sides of the 575px breakpoint, four variants,
disabled inheritance and child override, Form token overrides, compound noStyle
error aggregation, nested styled Items, stable List identities after a move, and
the placement of label/content/help/extra semantic padding and backgrounds.
`visual-reference.json` retains 40 selected logical-size metric cases; complete
metrics for every scale and scenario remain alongside the PNGs.

Browser and native use the same scene manifest. Native records must identify
`scene`, `theme`, `direction`, `scale`, `name`, and `viewport`, and contain ordered
`forms: [{id, root, items: [{id, root, label, control, input, help, extra, feedback, children}]}]`.
Every geometry entry is either null or an absolute logical-pixel rectangle with
`x`, `y`, `width`, and `height`. Additional measured `wrapper`, `row`, `label_text`,
and `tooltip` slots are also gated when supplied. The browser Form root excludes
the last Item's collapsed bottom margin; its scene wrapper includes that margin.
The mandatory feedback box measures the actual SVG glyph (14px default), matching
the native glyph element. The browser's additional `feedback_wrapper` preserves
its 22px inline carrier line box; its whitespace is not mistaken for glyph size.
Compound Items have recursive `children` records. A noStyle child's chrome slots
are null and its input box is measured; styled children use the full record.
The dynamic-list composition starts with Ada and Grace and executes move(1, 0),
so rendered row identities are row-1 then row-0, independently of their indexes.

Final native captures explicitly enable `test-support` for a passive label
prepaint callback. `label_text` measures the real GPUI logical Element bounds,
matching the browser's logical DOM rectangle; `painted_label_text` separately
retains the physical-pixel-snapped quad diagnostic. The callback adds no layout
node, style, or paint. `-WithoutLayoutInspection` builds the default feature set
for an independent pixel-equivalence capture.

Compare actual native renders after their capture:

```powershell
scripts/capture-form-native.ps1 -WithoutLayoutInspection -Theme default -Direction ltr -Scale 1 -Output ../build/ant-design-gpui/form-native-default
scripts/capture-form-native.ps1
python scripts/compare_form_instrumentation.py --plain ../build/ant-design-gpui/form-native-default --instrumented ../build/ant-design-gpui/form-native --output fixtures/form/instrumentation-validation-windows.json
node scripts/pack_form_visual_validation.mjs
```

The native evidence packer requires a coherent source-verified build, the normal
Windows 1 MiB main stack, a complete matrix, unchanged source and executable
hashes, and a successful original-source audit. It reruns comparison and all six
negative controls. Its compact report preserves every failed case and never
converts a known discrepancy into a passing gate. `-SkipBuild` captures support
exploratory diagnosis but cannot produce final attestation.
`instrumentation-validation-windows.json` binds both coherent feature builds to
the same source and verifies all 19 default-scene RGBA images are unchanged.

The comparator requires exact scenario and field identities, gates every required
slot's position and dimensions within one logical pixel, and gates solid painted
interiors within one RGB byte. It checks the entire viewport for unexpected
paint. Images are never aligned or resized. Reference glyph and SVG bounds and
one-logical-pixel antialiasing neighborhoods are excluded from solid-paint gates;
raw raster differences are reported, and paired screenshots remain necessary for
font/glyph review. The six synthetic negative controls verify shifted geometry,
missing input, incorrect fill, missing surface, extra outside paint, and missing
field rejection; they do not constitute a native parity result.
Two additional foreground-boundary controls accept a three-versus-four-byte
background contrast difference and reject three-versus-five, preserving the
same one-byte RGB budget in both the solid and coverage gates.
The source raster audit preserves the sampled single-physical-pixel fractional
viewport-edge bands at 575px / 150%; Chromium composites that edge over the white
body with tile-dependent coverage. This edge is part of the existing one-logical-
pixel raster antialiasing exclusion and does not relax component geometry gates.

`layout-probes.json` independently records genuine styled Items isolated from
their siblings, including one-line help (Item/Form 56px), two-line help (76px),
and help plus extra (80px). It preserves the negative margin-offset element and
shows that extra starts immediately after the 22px help text, with any remaining
24px spacing budget after the extra. `reference-validation.json` indexes verified
source hashes, captures, licenses, and comparator negative-control results.

`behavior-reference.json` preserves **13 independent executed traces**, including
real CDP input. They cover initialization/default precedence, silent setters and
user callbacks, touched metadata, errors, reset, named/selector/preserved watches,
conditional preservation, hidden validation, warning-only submission,
validate-only/dirty/recursive selection, dependencies, stable List keys and
insert/remove/move, overlapping async validation and reset, all three first-error
modes, and focused equal-value reset. `{"$undefined":true}` explicitly encodes
JavaScript undefined without conflating it with null or an omitted key.

`inheritance-reference.json` independently executes 15 configurations of 15
supported controls, with additional original package notices indexed in
`inheritance-packages.json`. It distinguishes Form context from ConfigProvider
context and verifies own-prop precedence, sizes, and variants. Pinned Segmented
ignores both Form.disabled and global componentDisabled, honors its own disabled
prop, and inherits Form size. Checkbox and Slider ignore Form size; Switch only
changes for small. ColorPicker inherits Form size and disabled but has no Form
variant. Input-family, numeric, select, date/range/time controls inherit variants.
Real pointer traces also preserve label focus and activation: Checkbox and Switch
labels toggle their controls and focus them; an Input label only focuses.

The upstream example section has 37 public demos and seven debug demos. Six
public compositions contain controls not currently provided by this GPUI library:
disabled (Cascader, TreeSelect, Upload, Rate, Mentions, Transfer, Tree), variants
(Cascader, TreeSelect, Mentions), size (Cascader, TreeSelect), registration
(Cascader, AutoComplete), static validation (Cascader, TreeSelect, Mentions), and
other controls (Rate, Upload). Form behavior for those compositions is exercised
with supported controls and custom adapters; substitution does not claim visual
equivalence for unavailable controls. Decorative layout primitives can be built
directly from GPUI elements without adding unrelated component implementations.

`switch-opacity-reference.json` retains 216 actual source handle-center pixel
samples from 12 captures over six uniform backdrops, both sizes and checked
states, disabled/default opacity, authored opacity, and decorative native
content. Its separate manifest does not alter the 19-scene Form matrix. Regenerate
the source with `form_switch_opacity_cases.mjs`, prepare the reference with
`--cases fixtures/form/switch-opacity-cases.json --output ../build/ant-design-gpui/form-opacity-reference`,
then capture that reference/manifest in default and dark themes at LTR/100%,
using `--append true` for the second theme; `pack_form_switch_opacity.py` records
the independently observed pixels. Native capture accepts the same manifest
through `-Manifest fixtures/form/switch-opacity-cases.json`.

Switch applies resolved root opacity after overlapping native content has
painted. Its declared backdrop defaults to `color_bg_container`; use
`Switch::opacity_backdrop(Color)` on a different opaque, uniform surface.
The pinned GPUI engine has no isolated subtree layer. A nonuniform backdrop,
ancestor opacity, or custom painting outside the layout box and declared shadow
extents remains outside this exact composition contract.
`opacity-backend-reference.json` records independent plain-CSS color probes
and the verified software-Skia integer interpolation model. Native floating-point
GPU blending can differ from those eight-bit software results by two bytes in
solid interiors at some authored opacities. Auxiliary strict comparison failures
are retained; the library does not special-case an opacity or browser backend.

`textarea-resize-reference.json` records 21 executed Chromium grip drags, including
disabled/read-only resizing, autoSize without a grip, physical RTL placement, and
preserved focus/selection. `probe_form_textarea_resize.mjs` retains the genuine
light/dark screenshots and their hashes under the ignored build directory.

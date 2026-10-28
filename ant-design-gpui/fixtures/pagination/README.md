# Pagination reference evidence

Reference fixtures execute the original Ant Design 6.6.7 Pagination from revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, with `@rc-component/pagination`
1.4.0. Developer dependencies reuse the locked local Select/Slider reference
workspace; the tooling does not install or download packages.

Regenerate the shared native/browser visual manifest and original browser build:

```powershell
node scripts/pagination_visual_cases.mjs fixtures/pagination/visual-cases.json
node scripts/prepare_pagination_reference.mjs --upstream E:/ant-design
node scripts/capture_pagination_reference.mjs --cases fixtures/pagination/visual-cases.json --output ../build/ant-design-gpui/pagination-reference/visual-150 --scale 1.5
node scripts/capture_pagination_reference.mjs --cases fixtures/pagination/visual-cases.json --output ../build/ant-design-gpui/pagination-reference/visual-100 --scale 1
node scripts/capture_pagination_reference.mjs --cases fixtures/pagination/visual-cases.json --output ../build/ant-design-gpui/pagination-reference/visual-200 --scale 2
node scripts/capture_pagination_behavior.mjs
node scripts/pack_pagination_reference.mjs
node scripts/capture_pagination_text_reference.mjs --scale 1
node scripts/capture_pagination_text_reference.mjs --scale 1.5
node scripts/capture_pagination_text_reference.mjs --scale 2
node scripts/pack_pagination_text_reference.mjs
node scripts/probe_pagination_go_paint.mjs --output ../build/ant-design-gpui/pagination-reference/go-paint-100 --scale 1
node scripts/probe_pagination_go_paint.mjs --output ../build/ant-design-gpui/pagination-reference/go-paint-150 --scale 1.5
node scripts/probe_pagination_go_paint.mjs --output ../build/ant-design-gpui/pagination-reference/go-paint-200 --scale 2
node scripts/pack_pagination_native_form.mjs
python scripts/audit_pagination_reference.py --reference ../build/ant-design-gpui/pagination-reference --output ../build/ant-design-gpui/pagination-reference/raster-integrity.json
```

Captures use a 1280 × 420 logical-pixel viewport by default, anchor `(64,48)`,
and a component width of `viewport_width - 128`. Narrow scenarios explicitly
set `viewport_width`. Each record preserves its exact scenario and measured DOM
geometry, computed styles, accessibility attributes, current page, and events.
Large images remain under the ignored build tree. Checked-in compact fixtures
include source hashes, package integrity, and browser version.

`visual-cases.json` has 416 scenarios, captured at 100%, 150%, and 200% native
scale. `visual-reference.json` stores the 150% browser measurements; captures at
the other scales remain in `visual-100` and `visual-200` under the ignored build
tree. `behavior-reference.json` contains 222 independently rendered page models
and 17 action/event traces, including controlled state, quick/simple editing,
ellipsis jumps, and page-size event ordering.

After building the native `pagination_visual` example, capture the same manifest
and compare the rendered surface primitives:

```powershell
cargo build --locked --offline --example pagination_visual
../build/ant-design-gpui/debug/examples/pagination_visual.exe --cases fixtures/pagination/visual-cases.json --output ../build/ant-design-gpui/pagination-native/visual-150 --scale 1.5
python scripts/compare_pagination_visual.py --native ../build/ant-design-gpui/pagination-native/visual-150 --reference ../build/ant-design-gpui/pagination-reference/visual-150 --output ../build/ant-design-gpui/pagination-native/comparison-150.json
```

Visual comparison uses at most one logical pixel of non-text geometry difference,
including effective corner radii, one byte of RGB rounding, and 0.001 alpha rounding.
An additional 68 total-label PNG foreground-line checks at each scale catch
unintended wrapping, including text overflow beyond an unchanged item box.
`text-reference.json` preserves 204 original DOMRange records (68 scenarios at
each scale), including line counts and character
positions; supplementary native paragraph-shaper probes check number/word visual
order against those original positions, including each wrapped line's full
paragraph context (124 comparisons per scale). Sixteen additional PNG line-band
checks per scale cover app-owned custom Previous/Next labels. Actual text and bidirectional labels are
also reviewed in the paired screenshots. Text glyph rasterization and shadows
are reported separately, without a claim of identical platform glyph pixels.

The browser's default confirmation button uses native form appearance. Chromium
reports a black two-pixel border in computed styles but paints a snapped gray
outline and neutral fill. `browser_painted_appearance` preserves those independently
measured pixel colors, outline width, and the source PNG hash; the comparator
checks this actual painted appearance while retaining DOM layout measurements.
The native form button's rounded outline is reviewed in the paired PNGs; its
misleading computed zero radius is excluded from the automatic CSS-radius gate.

The matrix includes 64 pointer/keyboard/disabled Go states, driven through real
browser and GPUI events. Each state preserves actual painted colors and source
pseudostates. Native colors are compared after compositing to the actual native
PNG, so translucent disabled paint is checked over both light and dark parents.
`native-form-reference.json` also preserves 30 independent samples over five
parent backgrounds and 12 live cursor records. Their effective affine fits
preserve the collapsed one-physical-pixel outline at 100%/150%; resolved 200%
profiles preserve the outer parent-backed and inner fill-backed border bands.
The original CSS RGBA is verified against those actual pixels; quantized
two-point fits are recorded as sample equivalents, not unique intrinsic alpha.
Seventy-two additional Go-label visibility checks per scale compare actual PNG
glyph contrast against the painted fill. Their font-antialiasing allowance is
the greater of eight RGB bytes or 25% of the reference contrast; this catches
background layers accidentally covering text without claiming identical glyph
rasterization or changing the non-text geometry/color budgets.

`capture-repairs.json` records three original-browser screenshot repairs (two at
150%, one at 200%): the initial PNGs contained repeated pager tiles. Fresh isolated
browsers rendered the same original source correctly, with byte-equivalent
DOM/style records and unchanged independent DOMRange evidence where applicable.
Original/replacement PNG hashes are preserved; corrupt images remain in the
ignored `repair-150` and `repair-200` directories. `audit_pagination_reference.py`
checks every source PNG for unexpected foreground outside measured root/popup
bounds, allowing normal shadows and inline font overflow.

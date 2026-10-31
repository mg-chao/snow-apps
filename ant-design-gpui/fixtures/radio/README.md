# Radio browser reference fixtures

`reference.json` contains **3,200 independent browser measurements** of Ant Design
6.6.7, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The four themes
(default, dark, compact, dark compact) are captured at rest, hover, active and
keyboard focus. Each capture has 104 standalone controls and 32 three-option
groups. The matrix covers circular radios, empty and wrapping labels, checked
and disabled combinations, wireframe mode, the original component-token demo,
all three button sizes, outline and solid buttons, horizontal and vertical
groups, block layout, shared border seams and corner placement, and both text
directions.

The generator executes the original `components/radio/style/index.ts`, including
its component token derivation, using the original `@ant-design/cssinjs` serializer
and CSS calculator. Concrete aliases come from the independently executed
[theme fixtures](../theme/README.md). Static DOM reproduces the original Radio,
Radio.Button and Radio.Group class/input/label structure, including the
`useCSSVarCls` root class and its common box-sizing reset. No Rust visual values
are used to generate the oracle. Deterministic data selectors activate the
serialized hover, active and focus-visible rules. Animations are disabled so
the fixture records settled visual states.

Headless Microsoft Edge measures `getComputedStyle` and element-relative bounds
using fresh isolated profiles. The fixture records the exact upstream revision,
package versions, source hashes, generator/capture hashes, theme-fixture hash
and browser version. Browser bounds are quantized to 1/64px; font rasterization
and text widths depend on the platform. Native offscreen tests separately
exercise actual GPUI layout, semantics, focus and interaction.

Regenerate using existing local packages and an installed browser:

```powershell
./scripts/capture_radio_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
```

Use `-EdgePath` for another Edge installation or `-OutputPath` for a comparison
fixture. Generate only HTML with:

```powershell
node scripts/generate_radio_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
```

No dependencies are installed or downloaded. Generated pages, raw DOM,
diagnostics and temporary profiles remain under ignored
`build/ant-design-gpui/radio-reference/`. Cargo needs only the checked-in JSON;
ordinary tests require no browser, Node.js, pnpm store or upstream checkout.
The original Ant Design MIT license is preserved in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

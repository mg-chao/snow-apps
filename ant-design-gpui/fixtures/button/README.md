# Button browser reference fixtures

`reference.json` records independently computed browser styles from Ant Design
**6.6.7**, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
It contains 12 cases: default, dark, compact, and dark compact themes, each in
rest, hover, and active states. Each case contains 184 controls covering legacy
types, all six variants, danger, sizes, shapes, icons, loading, disabled, ghost,
block, and all thirteen preset colors. The extended parity matrix exercises
every preset variant, all neutral variants, disabled preset variants, ghost and
disabled ghost combinations, and every size and shape with and without icons.
Together these compare 2,208 independently computed interaction states.

The generator executes the original Button token and style TypeScript and the
original `@ant-design/cssinjs` 2.1.2 serializer. Its adapters replace React hook
registration with style capture and supply concrete theme aliases from the
independent [theme fixtures](../theme/README.md). Button CSS is never read from
the Rust implementation. The adapter follows the real style hook's `rootCls`
default, applying common font resets to the Button root while nested icon slots
inherit its effective font size. Static DOM uses the source Button classes and pinned
Ant icon artwork. Decorative motion is disabled for capture. Hover and active
states substitute attributes for the original CSS pseudo-class selectors;
their declarations and disabled guards are unchanged.

Headless Microsoft Edge runs each generated page and records `getComputedStyle`
and layout bounds, including icon font size and artwork width/height. The fixture
includes the renderer version, viewport, device
scale, upstream revision, exact package versions, generator hashes, source
hashes, and theme fixture hash. The checked-in data was captured with Edge
156.0.4314.42 on Windows. Colors and token dimensions are the primary portable
assertions; text widths and rasterization also depend on fonts and renderer.
The gallery wrapper is only a comparison aid and is not an upstream component.

To regenerate from the crate directory using existing local packages:

```powershell
./scripts/capture_button_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
```

`-EdgePath` selects an existing Edge installation. `-OutputPath` writes an
alternate fixture for comparison. No packages are installed or downloaded, and
no existing browser profile is opened. Generated HTML, raw DOM, browser profiles,
and screenshots live under ignored `build/ant-design-gpui/button-reference/`.

For just the four reference pages:

```powershell
node scripts/generate_button_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
```

Open a generated HTML file with `?rtl`, `?state=hover`, or `?state=active` to
inspect those states. Ordinary Cargo tests consume only the checked-in fixture;
they require neither Node.js, Edge, the pnpm store, nor the upstream checkout.

The original Ant Design MIT license is preserved under
[`../theme/licenses/`](../theme/licenses/ant-design.txt). Icon attribution is in
[`../../resources/icons/README.md`](../../resources/icons/README.md).

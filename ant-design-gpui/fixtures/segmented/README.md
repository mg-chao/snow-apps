# Segmented browser reference fixtures

`reference.json` contains **576 independently measured Segmented states** (1,728
items) from Ant Design 6.6.7, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Four themes (default, dark, compact,
dark compact), both directions, and rest/hover/active/focus states cover three
sizes, default and round shapes, horizontal and vertical orientation, whole-group
and per-item disabling, block layout, icons, and component-local token overrides.

The generator executes the original `components/segmented/style/index.ts` with
the original `@ant-design/cssinjs` serializer. A hook adapter captures style
registration and supplies independently generated [theme aliases](../theme/README.md).
Static DOM follows the source's group, radio label/input and icon/label slots.
Headless Microsoft Edge records actual computed styles and bounds using fresh,
isolated profiles. Hover and active pseudo selectors become equally specific
attributes, preserving original CSS declarations. Each state loads in a separate
document before first layout so measurements do not sample transitions.

No values come from the Rust component. The fixture records upstream revision,
source/package/generator hashes, browser version and theme fixture hash. Tests
compare track/item geometry, state colors, typography, focus outlines, icon gaps,
all selected shadow layers and equal-width block items. Text widths and
antialiasing depend on platform fonts; native offscreen tests cover GPUI layout
and interaction separately.

Regenerate with existing local packages and an installed browser:

```powershell
node scripts/generate_segmented_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
```

Omit `--browser` to generate only the HTML references. `--output` changes the
generated-page directory; `--fixture` writes an alternate fixture. Packages are
neither installed nor downloaded. Generated pages, diagnostics and temporary
browser profiles stay under ignored `build/ant-design-gpui/segmented-reference/`.
Ordinary tests need only the checked-in JSON, with no Node.js, browser, pnpm store
or upstream checkout.

The upstream MIT license is retained in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

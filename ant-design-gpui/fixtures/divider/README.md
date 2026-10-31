# Divider browser reference fixtures

`reference.json` contains **592 independent browser measurements** of Ant Design
6.6.7, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Four themes
(default, dark, compact, dark compact) each contain 74 Divider examples in both
left-to-right and right-to-left directions. They cover horizontal and vertical
separators, all three variants, unset and explicit sizes, five title placements,
plain and heading typography, empty and zero titles, overflowing titles,
explicit title margins, and the source component-token customization example.

The generator executes the original `components/divider/style/index.ts` and the
original `@ant-design/cssinjs` serializer. An adapter captures style registration
and supplies concrete aliases from the independent [theme fixtures](../theme/README.md).
Static DOM follows the source component's classes and three-part rail/title
structure. Values are never read from the Rust Divider implementation. Headless
Microsoft Edge records `getComputedStyle` and root-relative bounds using a new,
isolated browser profile. The fixture records exact package versions, source
hashes, generator hash, upstream revision, renderer version, and theme-fixture hash.

The comparison test checks native visual metrics against the captured browser
values. The browser's inherited line-height multiplier is preserved for heading
titles: default 16px titles have a 25.1429px computed line-height. Measured rail
lengths verify the short-rail fraction independently of native layout. Browser
bounds are quantized to 1/64px; text widths and antialiasing depend on fonts and
the platform. The native offscreen tests cover the actual GPUI layout and semantics.

Regenerate using existing local packages and an installed browser:

```powershell
node scripts/generate_divider_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
```

Omit `--browser` to generate just the HTML references. `--output` changes the
generated-page directory; `--fixture` writes an alternate fixture for comparison.
No packages are installed or downloaded, and no existing browser profile is
opened. Generated pages, raw DOM, diagnostics, and isolated profiles remain under
ignored `build/ant-design-gpui/divider-reference/`. Cargo consumes only the
checked-in JSON; ordinary tests need neither Node.js, Edge, the pnpm store, nor
the upstream checkout.

The original Ant Design MIT license is preserved in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

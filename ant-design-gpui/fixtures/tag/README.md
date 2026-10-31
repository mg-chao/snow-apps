# Tag browser reference fixtures

`reference.json` contains **3,528 independent browser measurements** of Ant
Design 6.6.7, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
Default, dark, compact, and dark compact themes each contain 147 examples in
both left-to-right and right-to-left directions, measured at rest, hover, and
active states. Samples cover all three variants, all 13 preset colors, all five
statuses, five custom colors including translucent orange, disabled states,
checked and unchecked tags, leading and close icons, and empty content.

The generator executes the original Tag style generators, `useColor` hook,
`@ant-design/fast-color`, and `@ant-design/cssinjs` serializer. An adapter captures
style registration and React memoization; global aliases come from the independent
[theme fixtures](../theme/README.md). Static DOM follows the original Tag and
CheckableTag wrappers and icon/content/close siblings. No values are read from
the Rust implementation. Headless Microsoft Edge computes CSS and geometry with
fresh, isolated profiles. The fixture records the upstream revision, source and
package hashes, exact dependency versions, generator hash, browser version, and
theme-fixture hash.

The native comparison checks colors for every state and theme, typography,
dimensions, logical padding, radius, icon sizing and gaps, group spacing, and
direction. Recorded label width isolates font-specific metrics from native
geometry. Browser bounds are quantized to 1/64px; Chromium also rounds serialized
byte alpha, which is accounted for by the color comparison.

Two subtle source details are preserved: the close gap is 3px for plain text
but 7px after a wrapped label with a leading icon; empty tags have no line box
and collapse to their two 1px borders. Disabled unchecked tags stay transparent
on hover while their active state uses the disabled fill.

Regenerate with existing local packages and an installed browser:

```powershell
node scripts/generate_tag_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
```

Omit `--browser` to generate only HTML references. `--output` changes the generated
page directory; `--fixture` writes an alternate fixture. No packages are installed
or downloaded, and no existing browser profile is opened. HTML pages, raw DOM,
diagnostics, and isolated profiles stay under ignored
`build/ant-design-gpui/tag-reference/`. Cargo consumes only checked-in JSON and
needs neither Node, Edge, the pnpm store, nor the Ant Design checkout.

The original Ant Design MIT license is preserved in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

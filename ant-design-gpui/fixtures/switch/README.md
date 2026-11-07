# Switch browser reference fixtures

`reference.json` records **1,664 independently computed browser states** from
Ant Design **6.6.7**, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
The matrix covers default, dark, compact and dark compact themes; rest, hover,
keyboard focus and active press; normal and small sizes; checked and unchecked;
enabled, disabled and loading; empty, text, asymmetric text and fixed icon
content; and LTR and RTL. Eight additional probes per theme/state exercise all
fourteen component tokens, including negative track padding and multiple shadows.

The generator executes the original Switch style TypeScript, the original
`@ant-design/cssinjs-utils` CSS calculator and the original
`@ant-design/cssinjs` serializer. Only React hook registration and theme plumbing
are adapted. Concrete aliases come from the independently executed
[theme fixtures](../theme/README.md). No Rust declarations supply browser styles.

Static DOM matches the source Switch button, handle and both content spans from
the upstream snapshots. Microsoft Edge computes the CSS and layout. Attribute
substitution activates the source hover, focus-visible and active selectors while
preserving declarations, specificity and disabled guards. Component-token probes
are scoped with CSS `@scope` so descendant selectors and their specificity retain
the original structure. Decorative motion is disabled during capture.

The fixture records exact renderer/package versions, upstream source hashes,
generator/capture hashes and the theme-fixture hash. It was captured using Edge
156.0.4314.42 on Windows with device scale 1. Native tests compare colors, shadows,
dimensions, opacity, content margins and intrinsic sizing, handle positions,
pressed expansion, RTL content geometry, loading icons and focus outlines.
Chromium layout is quantized to 1/64 pixel; geometry comparisons allow that amount.

Both content states participate in intrinsic sizing, including the hidden state.
The empty control also retains the sum of its content padding if custom tokens
make that exceed its minimum width. The fixture retains the actual upstream
handle pseudo-element shadow while disabled/loading: the wildcard rule that
clears descendant box shadows does not clear the pseudo-element's own shadow.

To regenerate using existing local packages:

```powershell
./scripts/capture_switch_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
```

`-EdgePath` selects an installed Edge; `-OutputPath` writes an alternate fixture.
No packages are installed and no network is required. Fresh temporary browser
profiles, generated HTML and raw DOM remain under the ignored
`build/ant-design-gpui/switch-reference/` directory.

To generate only the inspection pages:

```powershell
node scripts/generate_switch_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
```

Append `?state=hover`, `?state=focus` or `?state=active` to inspect a state.
Ordinary Cargo tests consume the checked-in fixture without a browser, Node.js,
pnpm store or upstream checkout. The upstream MIT license is retained at
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

A repeated capture was byte-identical: SHA-256
`03d8ea9e0a3cf436122f3c100631657f4bb8e0619e2db801ec033a5c9b43d7cf`.

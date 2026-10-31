# Popup browser reference fixtures

`reference.json` contains 600 independently computed browser cases from Ant
Design 6.6.7, revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
The matrix includes Tooltip, Popover and Popconfirm; twelve placements; LTR and
RTL; default, dark, compact and dark compact themes; wireframe on/off; preset
colors; and confirmation titles with and without descriptions.

The generator executes the original TypeScript style generators, rounded-arrow
generator, CSS calculator and cssinjs serializer. React hook registration and
theme plumbing are adapted. Global aliases come from the independently executed
[theme fixtures](../theme/README.md). Rust component values do not supply any
browser styles. Static markup follows the original PurePanel structure, and the
warning icon comes from the pinned `@ant-design/icons-svg` package.

The fixture records upstream and package source hashes, exact renderer version,
script hashes, and the theme-fixture hash. Unit tests compare component colors,
typography, radii, padding, title spacing, confirmation spacing and rounded arrow
path coordinates against these browser values. Browser CSS serialization rounds
path coordinates, so comparisons allow 0.002 logical pixels.

These fixtures cover computed styles. They do not establish native screenshot
parity, motion parity, rc-trigger collision behavior, or Button appearance.
Buttons use dimension placeholders; the Button suite verifies that component.
Native screenshot, placement and motion checks are tracked separately in
[the implementation report](../../docs/popup-system.md).

Regenerate from existing local packages:

```powershell
./scripts/capture_popup_metrics.ps1 -Upstream E:/ant-design -PnpmStore E:/.pnpm-store/v10
```

`-EdgePath` selects an installed Edge and `-OutputPath` chooses another fixture
path. No network or package installation is required. Dedicated temporary browser
profiles, generated inspection HTML and raw DOM stay under the ignored
`build/ant-design-gpui/popup-reference/` directory. Cargo tests consume the saved
JSON without requiring a browser or upstream checkout.

## Native screenshot comparison

The same generator also writes 576 visual pages using the original Button style
generator for real small confirmation buttons. These cover four themes, wireframe,
both directions, all three components, and all twelve placements in an 800×500
logical-pixel viewport. Their ample-room positioning is deliberately simple;
these screenshots do not replace live rc-trigger collision fixtures.

```powershell
cargo run --locked --offline --example popup_visual -- --output ../build/ant-design-gpui/popup-native --all
./scripts/capture_popup_visual.ps1 -All
python scripts/compare_popup_visual.py --native ../build/ant-design-gpui/popup-native --reference ../build/ant-design-gpui/popup-reference/visual --output ../build/ant-design-gpui/popup-visual-comparison.json
```

The native harness uses a real hidden platform window and records its actual DPI.
Edge uses that DPI. The comparison reports logical bounds, pixel differences
around each popup, and a separate shadow-only region. It does not discard native
subpixel font rendering differences or average errors over the blank window.
Pillow and NumPy are required only for the comparison script. All screenshots
and reports remain under ignored `build/` paths.

The upstream MIT notice is retained in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

## Live placement comparison

`placement.json` contains 4,560 measurements from the real React/rc-trigger browser
implementation, using Ant's original placement generator. Cases cover twelve
placements, centered/edge-aligned/hidden arrows, both directions, viewport edges,
partially offscreen and narrow triggers, 8/16 px arrows, several popup sizes, custom radii and
disabled overflow adjustment. The Rust regression compares body positions,
effective placement and the automatic arrow's coordinate. Fixed edge arrows are
covered by the original CSS fixture instead.

The tooling is isolated from Rust dependencies and locked in `placement-tools/`.
To install it beneath the ignored build tree and regenerate:

```powershell
$fixtureTools = '../build/ant-design-gpui/popup-placement-tools'
New-Item -ItemType Directory -Force $fixtureTools | Out-Null
Copy-Item fixtures/popup/placement-tools/package*.json $fixtureTools
npm ci --prefix $fixtureTools --ignore-scripts --no-audit --no-fund
./scripts/capture_popup_placement.ps1 -Upstream E:/ant-design
```

The generator verifies both the Ant commit and the installed lockfile. Provenance
records original source hashes, package integrity, bundle hash and browser version.
Each measurement includes the browser's actual viewport; headless Windows may
impose a minimum window width. No Rust geometry supplies expected positions.
The MIT/Apache notices for this development tooling are in `placement-tools/licenses/`.

## Additional DPI, content and motion captures

After generating the original styles above, capture the default theme across all
placements and directions at 100% and 200%. The native harness sends DPI changes
to its own hidden Windows capture window without changing the monitor settings:

```powershell
cargo run --locked --offline --example popup_visual -- --output ../build/ant-design-gpui/popup-native-100 --placements --scale 1
./scripts/capture_popup_visual.ps1 -NativePath ../build/ant-design-gpui/popup-native-100 -VisualPath ../build/ant-design-gpui/popup-browser-100
python scripts/compare_popup_visual.py --native ../build/ant-design-gpui/popup-native-100 --reference ../build/ant-design-gpui/popup-browser-100 --output ../build/ant-design-gpui/popup-visual-100.json
```

Repeat with `--scale 2` and the `-200` output paths for 200%.
`visual-cases.json` supplies 105 shared input scenarios for native and browser
rendering: arrows, corners, wrapping, semantic refinements, radius and colors.
Each renderer independently derives styles and layout from these inputs.

```powershell
node scripts/generate_popup_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --extended true --output ../build/ant-design-gpui/popup-reference-radius
node scripts/generate_popup_extended.mjs E:/ant-design
cargo run --locked --offline --example popup_visual -- --output ../build/ant-design-gpui/popup-native-extended --cases fixtures/popup/visual-cases.json --scale 1.5
node scripts/capture_popup_extended.mjs ../build/ant-design-gpui/popup-native-extended
python scripts/compare_popup_visual.py --native ../build/ant-design-gpui/popup-native-extended --reference ../build/ant-design-gpui/popup-extended-browser --output ../build/ant-design-gpui/popup-visual-extended.json
cargo run --locked --offline --example popup_visual -- --output ../build/ant-design-gpui/popup-native-unique --unique
```

The extended browser capture uses Edge's debugging protocol in a dedicated
headless process, with an explicit 800×500 viewport at the native scale. It also
regenerates `motion.json`: ten timestamped position/size samples of the original
unique-container CSS transition, paused and sampled through Web Animations.
The Rust motion regression allows 0.025 px for browser layout rounding.
The native unique capture records actual sample times and GPU-painted bounds;
each sample starts from a separately settled source so image encoding cannot
delay a later sample. It is visual evidence, not a frame-rate benchmark.

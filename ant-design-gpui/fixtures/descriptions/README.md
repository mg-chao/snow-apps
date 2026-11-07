# Descriptions reference fixtures

The reference is the original Ant Design **6.6.7** source at revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, including Descriptions, ConfigProvider,
responsive hooks, and generated styles. The fixture harness imports that source;
it does not recreate the expected DOM or CSS.

`visual-cases.json` supplies the same inputs to browser and native renderers.
Its 32 scenes cover both layouts and border modes, all sizes, multiline and
unbroken text, spans and filled rows, thirteen viewport breakpoint boundaries,
partial breakpoint maps, zero and empty slots, colons, semantic overrides,
component tokens, nested components, intrinsic sizing, explicit cell widths,
dashed/dotted borders at 1/2/3 px, multilingual bidi text, and truncated/whitespace headers.
These follow the upstream `basic`, `border`, `vertical`, `vertical-border`,
`size`, `responsive`, and `block` demos, with probes from the debug `padding`,
`text`, `style`, `style-class`, and `component-token` demos.
The full capture matrix has 768 cases: four theme algorithms, two directions,
and three device scales (100%, 150%, 200%).

`reference.json` records 39 representative browser cases for independent unit
assertions. Measurements include root/header/table/row/cell rectangles, label
and content rectangles, text ranges, padding, borders, computed colors, and theme
tokens. It includes all default scenes, all four algorithms for bordered sizes,
and all four algorithms for the RTL horizontal scene. Its provenance hashes every
bundled source input and the existing checked-in developer dependency lock at
`fixtures/select/browser-package-lock.json`.
Both renderers record requested and actual viewport dimensions. Browser metrics
also retain `innerWidth`/`innerHeight`, the visual viewport, and actual xs/sm
media-query results. This exposes fractional native DIP rounding at 150% DPI
instead of treating requested dimensions as actual viewport measurements.

The browser has its own temporary profile and serves the original-source bundle
over a private loopback server. Native captures use a hidden application window.
The native renderer saves screenshots before a separate sentinel-color pass
measures semantic slot quads; measurement colors never appear in saved images.

```powershell
node scripts/prepare_descriptions_reference.mjs
node scripts/capture_descriptions_reference.mjs --fixtures true
cargo build --locked --offline --example descriptions_visual
../build/ant-design-gpui/debug/examples/descriptions_visual.exe --output ../build/ant-design-gpui/descriptions-native
python scripts/compare_descriptions_visual.py --browser ../build/ant-design-gpui/descriptions-browser --native ../build/ant-design-gpui/descriptions-native --output ../build/ant-design-gpui/descriptions-comparison.json
```

Capture scripts accept `--scene`, `--theme`, `--direction`, and `--scale` filters;
use a separate output directory for partial runs. Paired comparison requires
matching scenario sets. The geometry cutoff is 1.01 logical pixels (a one-pixel
target with 0.01 numeric allowance). Flat painted
interiors are gated within one color-channel unit, excluding reference text ranges
and antialiased edges. Raw unaligned image error and every geometry delta remain
in the report; rendering success alone does not imply visual parity.
`visual-validation-windows.json` summarizes the full 768-pair Windows run,
including all strict failures, measured residuals, the frozen native binary hash,
and the complete report's hash/path. It records 1,282 components passing both
gates and 326 strict failures; it does not claim universal visual parity.
Nested root/view rectangles are not independently gated natively; those scenes
have outer geometry, descendant-text masking, paint comparison, and manual review.
Whitespace-only title/extra content uses a layout-neutral observation fallback
for zero-area text. A zero-height header produces no GPU quad and is explicitly
marked ungated in the report; native offscreen tests check its presence and height,
while the complete root rectangle remains subject to the normal geometry gate.

For supplemental native-font performance evidence, add `--benchmark true` to a
filtered native capture. It times 30 forced `Window::refresh`/`Window::draw` calls
before the marker pass and records sorted samples, median, p95, build profile,
and text backend in each case's `performance` field. Screenshot encoding and GPU
completion waits are excluded. Use a Release build for performance conclusions;
Debug timings are diagnostic. These measurements use the real native text system,
complementing structural CPU benchmarks that use GPUI's test text backend.

The native build does not gain any new dependency. The existing locked browser
packages may be supplied with `--dependencies` to the preparation script. It also
finds the existing Select/Slider or Modal reference dependency cache automatically.
Browser PNGs, native PNGs, complete metrics, and bundles stay in the ignored build
tree. Generated metadata is evidence from the recorded renderer/platform, not a
claim of platform-independent glyph rasterization.

Ant Design's MIT notice is retained under `fixtures/theme/licenses/ant-design.txt`.
Shared browser package notices are retained by the existing reference fixtures.

# Result upstream and native verification

`reference.json` contains **1,312 independent browser measurements** of Ant Design
6.6.7 at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`: four themes (default,
dark, compact, dark compact), both directions, all seven statuses, all sixteen
combinations of title/subtitle/actions/body, custom and hidden icons, exception
illustration precedence, wrapping, unbroken text, numeric zero, rich content,
component token overrides, and all six semantic style slots.

The generator executes the **actual upstream `components/result/index.tsx`**,
all three original illustration components, the original Result style generator,
and the original cssinjs serializer. ReactDOMServer renders the component tree;
adapters supply ConfigProvider values and capture CSS registration. Global tokens
come from the independent theme fixture. No native Rust implementation values or
manually reconstructed Result markup enter these measurements.

The fixture records the upstream revision, loaded package versions and source
hashes, theme fixture hash, generator hash, and browser version. Cargo tests read
only checked-in JSON and need no browser, JavaScript runtime, package cache, or
reference checkout. Full original notices for all executed packages that ship
license files are retained in `licenses/`; the upstream Ant Design notice is
retained in `../theme/licenses/ant-design.txt`.

`visual-cases.json` drives **29 paired native/browser scenes**. It includes the
nine public Result examples plus component tokens, hidden icons, narrow and
unbroken text, rich elements, wrapped actions, mixed Arabic/English/CJK text,
plain newline and repeated whitespace, whitespace-only titles, mixed short words
and overlong tokens, plain-text extra content, and each exception illustration overriding a custom,
hidden icon. Four themes, both directions, and 100%, 150%, and 200% actual native
DPI produce **696 image pairs and 744 compared Results**.
The added `body-text` scene passes the same direct plain body string to upstream
Result and native Result, independently of the retained custom nested body cases.
Six narrow exception scenes cover 403/404/500 at root widths 200px and 260px,
including RTL block overconstraint and intrinsic SVG overflow at both edges.

The native harness saves each authentic screenshot, then performs a separate
marker-only render to measure the icon/title/subtitle/extra/body wrapper bounds.
Exception artwork bounds are read directly from the native full-color sprites.
Individual action bounds use a layout-neutral element delegate's prepaint bounds and are compared for
spacing, centering, RTL ordering, and wrapped-line placement.
The shared host-action fixtures use explicit 72/68px widths on both sides so
Result layout is measured independently of platform font advance hinting.
The original unconstrained host actions differed by **1.09896 logical pixels**
in intrinsic width at 150% DPI; the exact native/browser rectangles and renderer
provenance are retained in `host-action-metrics-windows.json`. These raw fixture
actions are not Ant Design Button measurements. The 1px Result geometry gate
does not claim unconstrained host action widths or Ant Design Button widths
meet that bound.

Remaining rendering differences are explicitly recorded in
`known-discrepancies-windows.json`: native GPUI uses a denser dashed border for
the semantic custom root, arbitrary nested host text retains stock GPUI bidi
behavior (including period placement in RTL), and browser/native font rasterization
differs. Result-owned plain body strings use the shared bidi paragraph path;
custom native subtrees retain their own text layout. The default component has
no dashed border. Full unmasked pixel errors retain all these differences.
Visual inspection covered every category, eleven additional dark compact RTL
representatives, and all six narrow exception categories in both directions.
GPUI discards empty-area paint primitives, so zero-height whitespace-only title
slots are explicitly recorded as unpainted, without inventing native bounds;
component context tests independently verify their semantic presence.
Marker colors never enter the screenshots. Comparisons retain signed native
minus browser deltas for the root and each semantic slot, and enforce a maximum
**1 logical pixel** geometry error. Physical image dimensions must match exactly.
Full-canvas pixel errors use no resizing, interpolation, font masking, or origin
alignment; a separately labeled aligned crop diagnostic can help isolate paint
differences. Browser versus GPUI text antialiasing can produce pixel differences
despite matching geometry.

The Browser plugin had no available browser on the validation machine. The
reproducible test harness therefore owns a fresh isolated headless Chrome process
and profile; it never attaches to an existing browser or reads user sessions.
Native captures own a uniquely titled hidden window and verify its platform DPI.
All generated HTML, browser profiles, PNGs, and full diagnostics stay under the
ignored `build/ant-design-gpui/` directory.

Regenerate using the existing pinned package cache and an extracted official
`@rc-component/util` 1.14.0 package, without installing packages:

```powershell
node scripts/generate_result_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_result_reference.mjs --output ../build/ant-design-gpui/result-browser
cargo run --example result_visual -- --output ../build/ant-design-gpui/result-native
python scripts/compare_result_visual.py --browser ../build/ant-design-gpui/result-browser --native ../build/ant-design-gpui/result-native --output ../build/ant-design-gpui/result-comparison.json --summary fixtures/result/visual-validation-windows.json
cargo test --lib result::
```

For a newly added scene with unchanged component sources, both capture tools
accept `--scene <name>` and merge those new measurements into existing captures.
The final validation reruns the complete matrix after component rendering changes.

Pillow is used only by the developer comparison script. Ordinary library users
and tests gain no JavaScript runtime dependency from this verification tooling.

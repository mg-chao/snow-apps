# Menu reference fixtures

`reference.json` records all **55 modern Menu component tokens** and **640
independently measured menus (3,968 rows)** from Ant Design 6.6.7, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. It covers default, dark, compact and
dark compact mapping algorithms; explicit light/dark Menu appearance; both
directions; vertical, inline, collapsed, horizontal and popup layouts; rest,
hover, active and keyboard focus; and component/global token overrides.

The generator executes the original `components/menu/style/index.ts` and its
style imports with the original CSS-in-JS serializer. A hook adapter captures
style registration and supplies independently generated
[global aliases](../theme/README.md). Static DOM follows the original item,
submenu, icon, label and extra slots. Equally specific attributes substitute
pseudo states, with motion disabled before initial layout. Headless Microsoft
Edge records actual computed styles and bounds using fresh browser profiles.
No fixture values come from the Rust implementation.

The fixture records upstream revision, input/package/generator hashes and browser
version. Tests compare every modern token and row geometry, typography, colors,
state precedence, active bars, keyboard focus, group titles and inline surfaces.
Native GPUI interaction and layout tests complement this static CSS reference.
Text widths and antialiasing remain platform dependent.

Regenerate using existing local packages and an installed browser:

```powershell
node scripts/generate_menu_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
```

Omit `--browser` to generate HTML and independently executed component tokens
under the ignored output directory, preserving the full checked-in fixture.
`--output` changes the generated-page directory; `--fixture` changes the
fixture destination. No packages are installed or downloaded. Generated HTML,
raw measurements, diagnostics and browser profiles stay under ignored
`build/ant-design-gpui/menu-reference/`. Ordinary tests need only the checked-in
JSON and do not require a browser, Node.js, upstream checkout or pnpm store.

The upstream MIT license is retained in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

## Executable browser/native screenshots

`visual-cases.json` defines 88 shared executable scenarios, including nested
flyouts, groups, collapsed icons and initials, responsive horizontal overflow,
extra content, danger selection, keyboard focus, hovered selected submenus,
pressed normal/danger/selected rows, custom expand/overflow indicators, mixed
submenu themes, token overrides and both text directions. Pressed states hold an
actual pointer-down event through capture; they do not emulate CSS attributes.
`edge-cases.json` separately checks empty menus, empty groups, a sole divider,
and a sole opened/closed inline submenu. Native root bounds are observed during
prepaint so zero-height menus are checked even when they paint no surface.
The original React implementation is bundled directly from the pinned checkout;
it uses the existing developer dependencies and lock maintained by
`prepare_select_slider_reference.mjs`, without modifying the checkout.

```powershell
node scripts/prepare_menu_reference.mjs --upstream E:/ant-design
cargo build --example menu_visual
node scripts/capture_menu_reference.mjs --cases fixtures/menu/visual-cases.json --output ../build/ant-design-gpui/menu-reference/visual-100/browser --scale 1
../build/ant-design-gpui/debug/examples/menu_visual.exe --cases fixtures/menu/visual-cases.json --output ../build/ant-design-gpui/menu-reference/visual-100/native --scale 1
python scripts/compare_menu_visual.py --native ../build/ant-design-gpui/menu-reference/visual-100/native --reference ../build/ant-design-gpui/menu-reference/visual-100/browser --output ../build/ant-design-gpui/menu-reference/visual-100/comparison.json
```

Repeat captures at scales `1.5` and `2` for 150% and 200% DPI. Native captures
exercise the Windows DPI-change notification on their own hidden window; no
monitor configuration is changed. The browser owns a fresh isolated profile.
The comparison enforces a one-logical-pixel budget for opaque surfaces and
focus outlines, checks rendered glyph/icon foreground colors, default-chevron
bounds and custom +/− indicator bounds, and reports
unmasked pixel errors for text, chevrons and shadows, with paired contact sheets
for visual inspection. Generated screenshots and metrics remain ignored
under `build/`; the fixture package itself has no runtime browser dependency.

Run the 9 edge scenarios separately with the same built reference and native
executable:

```powershell
node scripts/capture_menu_reference.mjs --cases fixtures/menu/edge-cases.json --output ../build/ant-design-gpui/menu-reference/edge-100/browser --scale 1
../build/ant-design-gpui/debug/examples/menu_visual.exe --cases fixtures/menu/edge-cases.json --output ../build/ant-design-gpui/menu-reference/edge-100/native --scale 1
python scripts/compare_menu_visual.py --native ../build/ant-design-gpui/menu-reference/edge-100/native --reference ../build/ant-design-gpui/menu-reference/edge-100/browser --output ../build/ant-design-gpui/menu-reference/edge-100/comparison.json
```

The complete screenshot matrix is 88 main scenarios plus 9 edge scenarios at each
of 100%, 150% and 200% DPI: 264 main pairs and 27 edge pairs. Pixel metrics include
all text, chevrons and shadows; paired images still require review for text
placement and rasterization because color/surface checks do not prove glyph layout. Default
chevrons are checked separately within their measured upstream CSS slots: a
foreground mask uses the dominant local background and relative contrast, then
compares all four painted edges within one logical pixel. The mask includes
coverage above 30% of local stroke contrast (with a 12-byte minimum), retaining
real antialiased edge pixels whose coverage differs between renderers. The
geometry budget stays one logical pixel. A retained pre-fix capture rejects the
old displaced chevrons with the same detector. The four custom +/− factory
scenarios have separate painted-bounds checks using their actual CSS content
boxes; a retained capture rejects all eight previously misaligned glyphs at
100% DPI. Arbitrary custom content still requires visual review. A visible
reference slot with no painted stroke fails comparison so a corrupt screenshot
cannot silently reduce coverage. Browser captures settle two animation frames
and the software compositor before screenshots; DOM metrics alone do not prove
that the captured software tiles contain the final frame.

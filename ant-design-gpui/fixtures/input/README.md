# Input browser reference measurements

`reference.json` records 8,064 controls measured from original Ant Design 6.6.7
style generators at revision `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.
Each of four theme pages (default, dark, compact, dark compact) has 672 controls,
measured at rest, hover and focus at 150% display scaling on Windows.

The matrix covers plain Input, TextArea and InputNumber in three sizes, four
variants, three validation states, enabled/disabled/read-only states and both
directions. Another 24 unmasked OTP examples cover size, variant and direction.
Styles are deduplicated; each control retains independent root, inner input and
numeric-action measurements. Missing parts are null.

The generator executes the original Input shared, component, textarea, OTP and
InputNumber style generators and their component defaults. It uses pinned local
packages for CSS-in-JS serialization and the independent theme fixtures. Static
DOM follows the recorded component sources. Deterministic selectors activate the
original pseudo-state styles; transitions are disabled. No native Rust visual
values are used to construct expected results. Source and package provenance is
in `provenance.json`; the Ant Design MIT license is in
[`../theme/licenses/ant-design.txt`](../theme/licenses/ant-design.txt).

Generate only the public fixture pages:

```powershell
node scripts/generate_input_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10
python -m http.server 8766 --bind 127.0.0.1 --directory ../build/ant-design-gpui/input-reference
```

Open each `http://127.0.0.1:8766/input-reference-THEME.html?state=STATE` in the
approved browser, using the four themes and three states above. The DOM script
element `#reference-metrics` contains the measurement JSON. Save its text as
`THEME-STATE.json` beside the generated pages, preserving `provenance.json`.
For the in-app Browser, extract controls in bounded chunks so the tool response
limit cannot truncate a page. Each page has 672 controls:

```javascript
const controls = [];
for (let start = 0; start < 672; start += 32) {
  controls.push(...await tab.playwright.evaluate(start =>
    JSON.parse(document.querySelector('#reference-metrics').textContent)
      .controls.slice(start, start + 32), start));
}
// Preserve theme, state and scale from the same DOM record when saving.
```

Pack the twelve results without recalculating any expected styles:

```powershell
python scripts/pack_input_reference.py --measurements ../build/ant-design-gpui/input-reference
cargo test --locked --offline --lib -- input_styles_match_original_browser_matrix
```

The focused test compares settled font/line metrics, padding, height, radius,
border width and colors across all 8,064 samples. Additional custom-token and
composite matrices are described below. Intrinsic OTP widths depend on the platform
font; native controls derive them from GPUI font metrics. These style comparisons
do not establish pixel-identical glyph rasterization. Backend text-layout limitations and
rendered scene checks are tracked separately in `VALIDATION.md`.

No browser, Node.js, upstream checkout or package store is required for ordinary
Cargo tests. Generated HTML and raw measurements stay in the ignored build tree.

## Custom tokens and composite controls

The `typography`, `geometry` and `colors` directories add **24,192** settled
measurements using the same 672-control matrix and four themes. Profiles are
defined in the generator: typography changes Input font sizes; geometry sets
fractional font sizes and explicit per-size padding; colors changes active/hover
backgrounds, borders and the focus shadow. The adapter invokes the original token
defaults with the patch, then applies explicit overrides, following the original
CSS-in-JS registration contract. OTP surrounding styles retain their own tokens.
`input_custom_tokens_match_original_browser_matrices` compares the same fields
as the base matrix. Generate and pack each profile separately:

```powershell
node scripts/generate_input_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --profile typography
# Capture the twelve input-reference-THEME.html?state=STATE pages in the typography directory.
python scripts/pack_input_reference.py --measurements ../build/ant-design-gpui/input-reference/typography --output fixtures/input/typography
```

`composite-default` and `composite-typography` add **20,160** measurements. Each
page contains 840 controls: affixed Input, Password, counted TextArea, icon Search,
labelled Search, joined Input/Button group and masked OTP, in both directions,
three sizes and four variants. Availability/status pairs are enabled/default,
enabled/error, enabled/warning, disabled/default and read-only/default. Each profile
has four themes measured at rest, hover and focus.

These pages execute the original Input/Search, Button/compact and Space.Compact
style generators. Static markup was audited against `@rc-component/input@1.3.1`;
source hashes are recorded in each provenance file. Circular SVG placeholders
measure icon boxes only. Base Button styles precede Search specialization, then
compact styles, without adding an extra reset for substyles. Ant's filled Search
hover background has equal-specificity rules whose result can depend on style
registration order; this fixture uses Button-before-Search, and GPUI consistently
uses the Search specialization. More-specific disabled rules retain precedence.

Generate a profile with `--output` pointing to a separate composite directory,
then capture its `input-composite-THEME.html?state=STATE` pages. Read the JSON
from `#composite-metrics` in bounded chunks (840 controls per page), and save as
`THEME-STATE.json` beside the generated provenance. Pack with:

```powershell
python scripts/pack_input_composite_reference.py --measurements ../build/ant-design-gpui/input-reference/composite-default --output fixtures/input/composite-default
cargo test --locked --offline --lib -- input_composite_styles_match_original_browser_matrices
```

The composite test compares frame height/font/border/background, search-button
height/icon width/font/padding/colors, affix/clear spacing, password-icon color,
and masked-OTP font/color. Raw measurements retain absolute positions; the portable
fixture deduplicates local dimensions and styles and records raw-file SHA-256
hashes. It does not assert every absolute child position or icon/glyph pixels.
Native offscreen tests additionally cover joined edges, focus layering, fractional
DPI, flex sizing, hidden-clear spacing, custom search height, mask centering and
constrained OTP widths. Final native captures cover light and dark/compact/RTL
scenes, including custom typography. Composite profiles do not exhaust arbitrary
user token combinations.

`motion.json` adds 1,440 timed samples across 96 cases from the four generated
`input-motion-THEME.html` pages. The page harness pauses native CSS transitions
and sets their animation time; it never calculates expected interpolation values.
It records forward samples at 0/50/100/150/200/250/300 ms and reversals after
50 ms at 0/25/50/75/100/150/200/300 ms. Save the completed `#motion-metrics` JSON
as `motion-THEME.json` beside the pages, then run the same pack command.
The generated page title ends in `complete` when all samples are ready.

The two `original_browser_timing` Rust checks compare frame colors and shadows,
clear/password icon colors, numeric handle reveal width/opacity/background and
step-button height/color/pressed background. They exposed CSS reversal shortening
and the clear icon's immediate focus-visible color. Both are now implemented.
Motion coverage uses the default tokens of each theme, enabled controls and
medium size. It does not replace native glyph or GPU presentation verification.
The provenance includes original source/package hashes and the two generator
harness hashes. Actual browser pointer events are disabled in fixture pages;
only authored state attributes activate the original styles.

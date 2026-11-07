# Spin upstream verification

`reference.json` records **728 independent browser measurements** of Ant Design
6.6.7 at `f8f5595ab5e891fff6a2a06c5b3b98af643d008e`: 91 cases in each direction
across default, dark, compact, and dark compact themes. Cases cover every size,
descriptions, zero/negative/positive/overflow progress, embedded content, stopped
loading, custom indicators, component tokens, and semantic styles.

The generator mounts the **actual upstream `Spin/index.tsx`, `Indicator`,
`Looper`, `Progress`, and `usePercent`** through ReactDOMClient. It executes the
original Spin style generator and cssinjs serializer. Small adapters supply
ConfigProvider values and capture style registration. The unit-bearing
`marginXXS` CSS variable preserves Ant v6's normal registration contract: inserting
a raw number into its dot-size CSS subtraction would produce invalid CSS.
Global tokens come from the independent [theme fixtures](../theme/README.md).
No Rust values or reconstructed Spin DOM enter these measurements.

The JSON also includes:

- **56 animation cases** at exact 0/200/400/800/1200/1600/2000 ms phases,
  across four themes and both directions. WAAPI pauses the original CSS
  animations and sets `currentTime`; dot opacity, bounds, and rotation matrices
  are measured directly. `layoutWidth`/`layoutHeight` report dimensions before
  transforms, while `width`/`height` report browser bounding rectangles.
- **Delay and auto-progress traces** executing the original React hooks with
  virtual host timers. React commits drain over two real browser frames. Traces
  cover cancellation before the deadline, exact deadline visibility, immediate
  stop, changed delay, unmount cleanup, 240 auto ticks, stop, and restart.
- **Three progress transitions**, 0→60, 60→90, and 90→0, sampled at exact
  0/75/150/225/300 ms. These retain actual CSS transition properties and easing.
  The first ring mounts fully visible while the dots fade and shrink. Returning
  to zero fades the ring without shrinking it: the later progress positioning
  transform overrides the generic hidden-holder scale rule.

`visual-cases.json` is shared by the browser and native GPU harnesses. Eight
scenes cover 28 rows; four themes, both directions, and 100%, 150%, and 200% DPI
produce **192 paired static images and 672 compared rows**. Static captures pause
the genuine CSS animations at zero and disable transitions. An additional
56 browser images preserve timed animation phases; those are excluded from the
static native comparison and checked numerically by deterministic native tests.

Each screenshot has a 740×1000 logical viewport. Physical images remain at their
original scale; comparisons must not resize them. Fullscreen Spin covers the
viewport. Embedded content uses a fixed 340×100 themed panel with a 1 px border,
6 px radius, 16 px padding, and the text “Loading content” at 14/22 px. The
reference dims content and applies its background wash without blur.

The additional **backdrops** scene measures the accepted difference between CSS
group opacity and the current native combined wash. Both renderers place the
same opaque themed panel over (1) uniform `#426e99` and (2) repeating 40-pixel
bands of `#e58d8d`, `#7aa8cc`, and `#d6c077`. The parent row is exactly 360×112;
the panel remains 340×100 with the same border, padding, text, and indicator as
the ordinary embedded scene. Stripes are drawn behind the component without
changing its geometry, reading screenshot pixels, aligning images, or filtering
either output.

For a flat interior panel pixel, CSS renders `0.5 B + 0.5 P`, where `B` is the
opaque container background and `P` is the visible parent background. Native
Spin's 70% container-color wash leaves that opaque pixel at `B`. The difference
is therefore expected wherever `P` differs from `B`; the stripe case also shows
why a single uniform correction cannot recover an arbitrary parent backdrop.
These 24 image pairs and 48 rows are retained in the raw comparison rather than
masked out or presented as matches. They document the chosen renderer limit;
the original seven scenes continue to measure the standard matching backdrop.
The two backdrop rows are visual-only and do not alter the 728-row numerical
fixture or its animation/behavior traces.

Regenerate using existing packages only; these scripts download and install
nothing. `@rc-component/util` 1.14.0 is the official package already extracted by
the Alert fixture workflow:

```powershell
node scripts/generate_spin_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_spin_reference.mjs --output ../build/ant-design-gpui/spin-browser
cargo run --example spin_visual -- --output ../build/ant-design-gpui/spin-native
python scripts/compare_spin_visual.py --browser ../build/ant-design-gpui/spin-browser --native ../build/ant-design-gpui/spin-native --output ../build/ant-design-gpui/spin-comparison.json --summary fixtures/spin/visual-validation-windows.json
cargo test --locked --offline --lib spin::reference_tests
cargo build --locked --offline --example spin_accessibility
pwsh -NoProfile -File scripts/test-spin-accessibility.ps1 -SkipBuild
```

The fixture records upstream revision, source/package hashes, package pins,
theme fixture hash, generator/capture hashes, and browser version. Ordinary Cargo
tests read only checked-in JSON. Generated HTML, client bundles, raw DOM,
screenshots, isolated browser profiles, and diagnostics stay under ignored
`build/ant-design-gpui/` directories. Retained package notices are in
[licenses/](licenses/); the original Ant Design MIT notice is retained in
[the theme licenses](../theme/licenses/ant-design.txt).

The native accessibility fixture activates normal AccessKit with a self-directed
`WM_GETOBJECT`; it never displays a window or sends global keyboard input. Seven
actual UI Automation states verify group busy/live/labels/descriptions/automation
IDs, progress-bar role and numeric range, indeterminate automatic progress,
progress removal, retained children, and the deferred fullscreen layer. Evidence
is retained in `accessibility-validation-windows.json`; raw trees stay under the
ignored `spin-uia` output directory. The pinned GPUI debug JSON omits busy, live,
and author-ID fields, so those properties are queried through actual UI Automation
COM properties.

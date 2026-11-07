# Result

Native GPUI implementation of Ant Design 6.6.7 Result, reference revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Rendering uses GPUI layout,
native text shaping, and the existing bounded icon renderer. JavaScript is used
only by the independent developer reference tools.

## Composition

Build Result on each render with a stable ID and the applicable configuration scope:

```rust
use ant_design_gpui::{Button, Result, ResultStatus};
use gpui::prelude::*;

let result = Result::new("purchase-result")
    .status(ResultStatus::Success)
    .title("Successfully Purchased Cloud Server ECS!")
    .sub_title("Your cloud server configuration will be ready shortly.")
    .extra(Button::new("console").label("Go Console"))
    .extra(Button::new("purchase-again").label("Buy Again"))
    .child("Additional order information");
```

The default status is `Info`. The remaining statuses are `Success`, `Warning`,
`Error`, `Forbidden` (403), `NotFound` (404), and `InternalServerError` (500).
Status selects artwork and color; it does not supply text or actions. Copy is
application-owned and should use the application's localization system.

`title` and `sub_title` accept text. Their `title_content` and `sub_title_content`
counterparts accept native elements. Setting either form replaces the preceding
form of that slot. Empty text omits its slot. Repeated `extra` calls append direct
action children, while `child`/`children` populate the body below the actions.
Custom content owns its interactions and accessibility.

Plain text follows the reference's normal whitespace and word-breaking rules:
ASCII whitespace collapses, Unicode line-break opportunities wrap text, and
unbroken words may overflow. Original logical text remains accessible. Supply
custom native content when explicit paragraph breaks are intended.

`icon` accepts an `IconRef` or built-in `IconName`; `icon_content` accepts native
elements with their own dimensions. `show_icon(false)` removes the ordinary
status icon and its wrapper. Following the reference, 403/404/500 always display
their illustration even when a custom or hidden icon is requested.
`ResultStatus::illustration()` exposes those full-color artwork references for
reuse outside Result. Artwork keeps its original colors and orientation in dark
and RTL layouts.

Result is a static accessibility group without a tab stop or live announcement.
It exposes role, label, description, and automation-ID overrides. Buttons and
other supplied controls retain normal keyboard focus and activation.

## Styling and lifetime

The six `ResultStyles` slots are `root`, `icon`, `title`, `sub_title`, `extra`,
and `body`. Each is a native `StyleRefinement`. Styles resolve in this order:
scoped styles, scoped resolver, instance styles, instance resolver, and the
root's ordinary `Styled` refinements. Resolvers receive effective status,
direction, icon visibility, and slot-presence information.

`ConfigPatch.result` supplies inherited semantic styles and an optional
`ResultStyleResolver`. Retain or clone resolver wrappers when editing a scope;
identity equality lets no-op updates preserve their shared snapshots.

`ResultTheme` exposes `ResultTokenPatch` with `title_font_size`,
`subtitle_font_size`, `icon_font_size`, and `extra_margin`. `ResultMargin`
represents physical top/right/bottom/left logical-pixel margins, including zero
and negative values. Global tokens supply padding, colors, and line heights;
component token overrides use the existing component-theme system.

Native block layout preserves adjacent collapsing margins. Text and actions
wrap within the available width. Exception illustrations retain their original
251/252/254 by 294 dimensions inside the reference's 250 by 295 wrapper, including
its intentional horizontal overflow. Very narrow containers may therefore
overflow just as the reference does.

The only retained component state observes its configuration scope. Changing
scope replaces that subscription. Static Results schedule no animation frames;
the shared text helper retains bounded width variants, and full-color SVGs use
the existing bounded parse/raster cache. Result installs no asset source. Its
CSS-compatible word wrapping directly reuses `unicode-linebreak` 0.1.5, already
present in the locked GPUI dependency graph, instead of duplicating Unicode
line-breaking rules.

## Focused verification

```powershell
cargo test --locked --offline --lib result::
cargo test --locked --offline --lib config::tests::result_scopes
cargo test --locked --offline --lib behavior::text::
cargo test --locked --offline --lib alert::tests::alert_wrapping_and_custom_content_use_actual_native_layout -- --exact
cargo test --locked --offline --example gallery result_gallery
cargo run --locked --offline --example gallery -- --component result --headless --reduced-motion --capture ../build/ant-design-gpui/result-gallery.png
pwsh -NoProfile -File scripts/run-result-perf.ps1
```

The performance runner requires the repository's `windows-msvc-performance`
configuration with Release and benchmarks enabled. It writes
`build/windows-msvc-performance/ant-design-gpui/result-performance.json` and
checks zero idle callbacks and no additional parse/raster misses in the shared
illustration renderer during warm redraws. It measures CPU/offscreen drawing
rather than native GPU latency.
The deterministic GPUI test text system also excludes production operating-system
font shaping and glyph rasterization latency. Native Windows text appearance is
verified independently by the GPU capture matrix.
The matrix covers 1/20/100 mounted Results at each of three display scales and
both directions, with ordinary, illustrated, and rich content. First-draw values
are single observations after changing mode or direction; RTL can reuse earlier
LTR caches. Warm values report median and 95th-percentile durations over 100
forced redraws.

Independent browser fixtures, source provenance, visual measurements, and
reproduction commands are documented in `fixtures/result/README.md`. Generated
pages, screenshots, and full diagnostic reports stay under the ignored build
directory. Cross-platform native validation must be recorded separately.

## Windows verification, 2026-10-28

- **36 focused library tests and four gallery tests passed.** These include 952
  independent browser slot comparisons, narrow exception layouts at each scale,
  native keyboard actions, style precedence, scope replacement, and cache lifetime.
- **696 browser/native image pairs covering 744 Results passed** the presence,
  physical-image-size, and one-logical-pixel geometry gates. Maximum root, slot,
  illustration, or action-bounds difference was **0.515625 logical pixels**.
  The matrix covers all four themes, both directions, three display scales,
  all public example categories, and 200/260px exception layouts.
- **54 focused Release benchmark cases passed:** zero idle animation callbacks,
  no additional illustration parse/raster misses after warmup, and shared
  artwork across mounted instances. Peak tracked raster data was 3,560,928 bytes;
  this excludes the GPU atlas and is not a whole-process memory measurement.
- Strict Clippy and scoped Rust formatting passed. No full test suite ran.

Machine-readable evidence is in
[visual validation](../fixtures/result/visual-validation-windows.json),
[performance measurements](../fixtures/result/performance-windows.json), and
[verification details](../fixtures/result/validation-windows.json).

The [remaining differences](../fixtures/result/known-discrepancies-windows.json)
are explicit: Windows/browser font metrics and antialiasing differ; native
custom dashed borders use a denser pattern; and raw text inside arbitrary custom
GPUI subtrees can retain different RTL punctuation placement. Direct Result text
children use the verified paragraph path. Equivalent fixed-width action probes
isolate Result layout from host-control font advances; the original unconstrained
host-width discrepancy is retained separately.

macOS/Linux native rendering, production GPU latency, and production font-shaping
performance were not verified. The whole-project formatting check reports
pre-existing CRLF files outside this change; the touched Rust files pass the
repository's LF formatting rules. Whole-page gallery captures are limited by
native window-size clamping; focused component captures and offscreen gallery
tests cover the remaining categories.

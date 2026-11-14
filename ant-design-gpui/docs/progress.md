# Progress

`Progress` is a controlled GPUI element. Keep its ID stable and store the percentage
in the owning view. Internal keyed state retains only transitions, subscriptions,
and paint caches; applications do not need a progress controller or timer.

```rust
use ant_design_gpui::{Progress, progress::*};
use ant_design_gpui::theme::Color;

let progress = Progress::new("download")
    .percent(65.)
    .success(ProgressSuccess::new(30.).color(Color::rgb(0x52c41a)))
    .accessibility_label("Download progress")
    .automation_id("download.progress");

let dashboard = Progress::new("processing")
    .kind(ProgressKind::Dashboard)
    .percent(80.)
    .gap_degree(75.)
    .gap_placement(ProgressGapPlacement::Bottom)
    .stroke_color(ProgressGradient::new([
        (0., Color::rgb(0x108ee9)),
        (0.5, Color::rgb(0xffe58f)),
        (1., Color::rgb(0x87d068)),
    ]));
```

## Values and appearance

- `kind`: line, circle, or dashboard. The default is line.
- `percent`: finite values are clamped to 0–100; NaN becomes zero. A success
  segment has an independent percentage and optional color. An explicit status
  overrides automatic success; otherwise success is determined by the success
  percentage when supplied, or the total percentage.
- `status`: normal, active, exception, or success. Active lines use the native
  animated sweep. Circular active indicators retain the normal appearance.
- `size`: `ProgressSize::Small`, `Medium`, a numeric size, or dimensions.
  Circles default to 120 pixels (60 for small). Lines default to 8 pixels tall
  (6 for small). `ProgressSize::Dimensions` supports native relative widths:
  `width: Some(gpui::relative(0.75).into())`. Array sizes convert with `.size([300., 20.])`.
- `stroke_width`: circular stroke thickness as a percentage of the diameter.
  Line thickness belongs in `size`. The browser's deprecated line `strokeWidth`
  and `trailColor` aliases are not duplicated; use `size` and `rail_color`.
- `stroke_linecap`: round, butt, or square. Circular gradients use butt caps,
  matching the reference.
- `stroke_color`: a solid color, shared immutable gradient, or step color array
  (`ProgressStroke::steps`). Gradient stops are fractions, not percentages;
  positions are sorted and clamped, and nonfinite positions are discarded.
  `.angle(degrees)` follows CSS angles for lines: 0 points up, 90 right.
- `steps`: a count or `ProgressSteps { count, gap }`. For circles, gap is in the
  reference's 100-unit viewbox. Line steps use the theme's fixed gap; dimensions
  describe a single step. `.rounding(f64::floor)` customizes line-step rounding.
- `gap_degree` and logical `gap_placement`: dashboard/circle gap, clamped to
  0–295 degrees. Start and end follow the scoped writing direction.

## Labels and semantics

`show_info(false)` removes visible information without removing the accessible
range. `format` returns text; `format_content` returns any native element. The
factory is called once per render. Inner content is measured intrinsically so
it fits even at zero percent. `ProgressPercentPosition` chooses inner/outer and
start/center/end placement. Outer center places the information below the rail.
For diameters at or below 20 pixels, information moves into the shared Tooltip.
Mount `UiRoot` to provide the tooltip host.

Progress exposes one nonfocusable, read-only progress indicator with the current
clamped numeric value, minimum zero and maximum 100. Supply a localized
`accessibility_label` and optional `accessibility_description`; `automation_id`
is a stable untranslated identifier. The native value retains fractional
precision (the browser reference truncates its ARIA value).

## Configuration and native styling

`.scope(scope)` inherits theme, direction, font, motion and `ProgressConfig`.
Provider styles are followed by provider resolver, local styles, local resolver,
and root `Styled` refinements. Resolvers receive the current kind, resolved status,
percentage, success percentage, size, steps, information placement, direction,
and motion policy.

`ProgressStyles` has `root`, `body`, `rail`, `track`, and `indicator` slots.
Root, body and indicator use ordinary GPUI styles. Line rail and track accept
native layout refinements. Painted circle/step rail and track interpret paint
properties (background and opacity); use `size`, `stroke_width`, `steps` and
`gap_degree` for their geometry. These paint slots are not DOM nodes.

`ProgressTokenPatch` provides six component tokens: `default_color`,
`remaining_color`, `circle_text_color`, `line_border_radius`,
`circle_text_font_size`, and `circle_icon_font_size`. The two font tokens are
relative factors. Register patches through `components.set::<ProgressTheme>(...)`.

Native paths and gradient sections are cached independently of their position.
Optional style overrides are allocated lazily, keeping ordinary builders small
enough to compose safely on a native GUI thread's stack.
Solid color transitions reuse geometry. Circular steps group adjacent equal-color
cells into shared masks; line steps realize only the visible range. Extremely
dense circular steps below device-pixel resolution aggregate their coverage
across the whole ring. Resting controls request no frames, and animations stop
scheduling when hidden, clipped, unmounted, or reduced motion is requested.

The gallery includes all sixteen public reference examples plus a token example:

```powershell
cargo run --locked --offline --example gallery -- --component progress
cargo test --locked --offline --lib progress::
cargo test --locked --offline --example gallery progress_gallery
./scripts/test-progress-accessibility.ps1
./scripts/run-progress-perf.ps1
```

See [verification](progress-verification.md) for measured coverage and limitations.

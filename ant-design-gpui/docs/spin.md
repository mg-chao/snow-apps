# Spin

Native GPUI implementation of the Spin component in Ant Design 6.6.7. Layout,
text, indicators, progress rings, and overlays use GPUI; no web view or runtime
JavaScript is involved. The reference is the local `E:/ant-design/components/spin`
implementation. Browser fixtures are developer verification tools only.

## Composition

Rebuild Spin with a stable ID on each render:

```rust
use std::time::Duration;
use ant_design_gpui::{Size, Spin};
use gpui::prelude::*;

let standalone = Spin::new("loading").size(Size::Small);
let nested = Spin::new("loading-results")
    .spinning(loading)
    .delay(Duration::from_millis(500))
    .description("Loading results...")
    .child(gpui::div().child("Results remain mounted while loading"));
```

Spin defaults to spinning, medium size, no delay, no description, and the four-dot
indicator. Size inherits the scope's control size and otherwise defaults to medium;
an explicit size selects small, medium, or large. `description` accepts application-localized text;
`description_content` accepts a native element. The latter owns its accessibility
and interactions. `child` and `children` enable embedded mode. Fullscreen mode
uses a window-wide backdrop; supplied children render inside that viewport layer.

The delay starts when loading is requested. Turning loading off cancels a pending
delay; unrelated renders do not restart it. Loading already shown remains visible
when its delay changes. Retained state and timer handles are released on unmount.

`percent(35.)` or `percent(SpinPercent::Value(35.))` draws measured progress;
`percent(SpinPercent::Auto)` advances an estimate while waiting. Numeric progress
is clamped to the supported range. The default dots and progress ring transition
between their reference states. A custom indicator replaces that visual entirely.

## Indicators and styling

`indicator` accepts a built-in `IconName`, an `IconRef`, or a `SpinIndicator`.
`indicator_content` accepts native content. Custom icons remain static unless
`indicator_spin(true)` or `SpinIndicator::animated(true)` opts them into animation.
For icon references, Spin applies the rotation. For content factories, animation
requests frames and supplies the current angle through `SpinStyleContext.rotation`;
the factory uses that value to render its own transforms. Spin does not rotate an
arbitrary native element subtree.

`SpinIndicator::from_content` builds a native element from the effective style
context, window, and application. Configured factories run during rendering,
never during configuration resolution. Retain or clone the wrapper to preserve
identity across no-op scope changes. `ConfigPatch.spin.indicator` supplies a scoped
default, replacing the reference's process-wide mutable default setter.

The five `SpinStyles` slots are `root`, `section`, `indicator`, `description`, and
`container`. Each uses native `StyleRefinement`; `styles_fn` resolves styles from
effective size, spinning state, fullscreen mode, direction, percentage, motion,
and whether embedded children exist. Scoped styles and resolvers are inherited,
then instance styles and resolvers apply; ordinary `Styled` refinements customize
the root. Class names have no native equivalent.

`SpinTheme` supports `SpinTokenPatch` fields `content_height`, `dot_size`,
`dot_size_sm`, and `dot_size_lg`. Global aliases supply colors, spacing, fonts,
motion duration, and fullscreen layer priority. Spin honors disabled motion and
application reduced motion, keeping a static loading visual without continuously
requesting animation frames. Inactive, delayed, and clipped indicators do not
keep an animation loop running.

## Native behavior and visual limits

Embedded content stays mounted and preserves its layout and state. Its loading
overlay blocks pointer interaction, hover, and scrolling; keyboard focus and
accessible actions retain their existing behavior, matching the reference's
pointer-only treatment. Spin is not a modal focus trap. Disable application
controls explicitly when an operation must prevent keyboard activation as well.
Spin itself has no tab stop or dismissal key. Its loading state and measured
progress are exposed through native accessibility metadata.

The current reference uses opacity and a container-color wash, with no blur.
GPUI opacity multiplies individual primitives rather than compositing an isolated
subtree. Spin therefore draws its children normally and uses the equivalent
combined wash: at rest in the loading state, 70% container background reproduces
the reference's 50% group opacity with a 40% inner wash when the surrounding
backdrop equals the container background. Patterned, gradient, or differently
colored surrounding backdrops can differ from browser group compositing. This
implementation keeps the existing GPUI revision and avoids subtree snapshots or
expensive CPU/GPU readbacks.

Fullscreen overlays escape ancestor clipping and follow the existing deferred
layer system. The default layer is `zIndexPopupBase`; ordinary popovers and
tooltips have higher default layers, as in Ant Design. At equal configured
layers, PopupHost-managed popup ordering takes precedence over Spin. Multiple
Spin overlays follow their stable deferred draw order. Fullscreen displays cover
the GPUI client viewport, leaving operating-system window decorations intact.

## Focused verification

```powershell
cargo test --locked --offline --lib spin::
cargo test --locked --offline --example gallery spin_gallery
cargo run --locked --offline --example gallery -- --component spin --headless --reduced-motion --capture ../build/ant-design-gpui/spin-gallery.png
pwsh -NoProfile -File scripts/run-spin-perf.ps1
```

The gallery follows the nine public reference demos in order. Embedded and delay
examples have independent loading switches. Progress can run to completion or
use automatic mode; its measured demonstration is finite and starts on demand.
The fullscreen example completes automatically and cancels its previous timer
when restarted. Neither demo uses an endless page-level timer while idle.

Use the gallery's dark, compact, RTL, and reduced-motion controls for presentation
checks. Spin-only tests exercise timer cancellation, transitions, native layout,
configuration, accessibility, and idle scheduling. Run only related tests; a full
repository test run is unnecessary for this component.

See [Spin verification](spin-verification.md) for native capture commands,
browser comparisons, measured visual differences, and performance evidence.

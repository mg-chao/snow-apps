# Alert

Native GPUI implementation of Ant Design 6.6.7 Alert, reference commit
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Layout, icons, focus and dismissal
run in GPUI. The reference checkout and JavaScript packages are developer tools
for regenerating the independent browser fixtures.

## Ownership and dismissal

Build a fresh `Alert` during render with a stable, locally unique ID. Its small
private state retains dismissal, close focus, configuration observation and motion.
No application-owned controller is required.

```rust
use ant_design_gpui::{Alert, AlertType, ConfigScope};

fn notice(scope: ConfigScope) -> impl gpui::IntoElement {
    Alert::new("upload-result")
        .scope(scope)
        .alert_type(AlertType::Success)
        .title("Upload complete")
        .description("The document is ready to share.")
        .show_icon(true)
        .closable(true)
        .on_close(|activation, _window, _cx| {
            // Observe the accepted pointer, keyboard or accessibility activation.
            let _ = activation;
        })
        .after_close(|_window, _cx| {
            // The owner may now remove its completed notice from a collection.
        })
}
```

`on_close` runs immediately after dismissal is accepted. It cannot cancel closing.
Repeated activations during the leave transition do not emit another close callback.
`after_close` runs once when the transition completes and the Alert disappears.
Callbacks run without a mutable borrow of the private state, allowing the owner to
update its own view. Removing the Alert before completion cancels `after_close`.
Keep it mounted until that callback when the owner wants the full leave animation.

Closing persists across rerenders with the same ID. Unmount and remount, or use a
new ID for a new notice, to create an open Alert again. Updating its title alone
does not reopen it.

The leave transition derives max-height from the actual styled open bounds and
animates max-height, opacity and bottom margin over the theme's slow duration with
circular easing. Default title-only vertical padding also collapses. Description
padding and explicit root padding remain in place during leave, following the
reference's CSS specificity, and disappear when the Alert is removed.
Scoped motion settings and GPUI reduced motion snap
directly to the final state while preserving `on_close` before `after_close`.
Open and settled Alerts request no animation frames.

## Content and behavior

| Area | API and defaults |
| --- | --- |
| Status | `AlertType::{Success, Info, Warning, Error}`; ordinary Alerts default to Info |
| Variant | `AlertVariant::{Outlined, Filled}`; Outlined by default; Filled retains a transparent border of the same width |
| Banner | `banner(true)` defaults to Warning with an icon, removes the border, and defaults to square corners; explicit type and icon visibility still apply |
| Title and description | `title` and `description` accept text; `title_content` and `description_content` accept native `IntoElement`; empty text omits its slot |
| Status icon | `show_icon` controls visibility; `icon` accepts `IconRef`/`IconName`, while `icon_content` accepts native content; setting an icon does not itself show it |
| Actions | `action` accepts native controls or content without coupling their activation to dismissal |
| Close | `closable`, `close_icon`, `close_content`, `close_disabled`, `close_focus_handle` and explicit accessible labels |
| Observation | `on_click` and `on_hover`; observing clicks does not make the Alert a keyboard button |

Custom close content enables closing unless `closable(false)` is explicit.
`close_disabled(true)` disables only the close control. This is an instance setting;
the general disabled provider does not disable an Alert's close control. Action
controls retain their own availability rules.

Slots use native layout; titles and descriptions wrap at constrained widths,
and application controls determine their own dimensions. RTL reverses logical
icon/action/close placement and aligns text to the right. Provider input size
does not select a separate Alert size;
the compact theme changes the tokens used for its geometry.

## Scoped defaults and customization

`ConfigPatch::alert` supplies an `AlertConfigPatch`: variant, closability, close
label, close icon, four status icons, static semantic styles and an optional
semantic-style resolver. Explicit instance settings override inherited defaults.
An unset patch field inherits its parent; resetting a local override restores that
inheritance. A configured icon alone does not enable close-button visibility.

```rust
use ant_design_gpui::{AlertConfigPatch, AlertIcon, AlertVariant, ConfigScope};
use ant_design_gpui::icons::IconName;

fn configure(scope: &ConfigScope, cx: &mut gpui::App) {
    let success_icon = AlertIcon::new(IconName::SmileOutlined);
    scope.update_patch(cx, |patch| {
        patch.alert = AlertConfigPatch {
            variant: Some(AlertVariant::Filled),
            closable: Some(true),
            success_icon: Some(success_icon),
            ..Default::default()
        };
    }).unwrap();
}
```

`AlertIcon` and `AlertStyleResolver` compare by shared identity. Retain and clone
them for repeated configuration updates so unchanged defaults remain no-ops.
The icon wrapper accepts `IconRef` or `IconName`. `AlertIcon::from_content` accepts
a factory with the resolved `AlertStyleContext`, window and app, returning native
`IntoElement` content for each Alert. Factories support rich status icons and close
content in scoped defaults; they run during rendering and never during configuration
merging. Retain application state in entities rather than in a generated element.
`reference()` and `render_content(...)` expose the two source forms to integrations.
`AlertStyleResolver::new` receives the same context, window and app.
Scoped factories and style resolvers require `Send + Sync` so shared configuration
snapshots preserve their thread-safe ownership. Capture shared data through `Arc`
when needed. Instance `styles_fn` callbacks remain local to the GPUI UI thread and
can capture `Rc` state.

```rust
use ant_design_gpui::{AlertConfigPatch, AlertIcon};
use gpui::{div, prelude::*};

let close_content = AlertIcon::from_content(|_context, _window, _cx| {
    div().child("Dismiss")
});
let defaults = AlertConfigPatch {
    close_icon: Some(close_content),
    ..Default::default()
};
```

`AlertStyles` exposes `root`, `icon`, `section`, `title`, `description`, `actions`
and `close`. Apply `.styles(...)` for static refinements or `.styles_fn(...)` for
styles derived from the effective status, variant, banner, icon visibility,
closability, description presence and direction. Individual properties merge in
this order, with later values taking precedence:

1. Inherited and local scope static styles.
2. The scope's effective style resolver.
3. Instance static styles, then its style resolver.
4. GPUI `Styled` refinements on the instance root.

Banner borders remain absent. Dismissal owns animated max-height, opacity and
bottom margin while leaving; the reference's description/explicit-padding
precedence still applies.

Component tokens use `AlertTheme` and `AlertTokenPatch`. Padding uses the typed
`AlertPadding::{new, symmetric, all}` constructors in logical pixels.

```rust
use ant_design_gpui::alert::{AlertPadding, AlertTheme, AlertTokenPatch};
use ant_design_gpui::theme::ThemeConfig;

let mut theme = ThemeConfig::default();
theme.components.set::<AlertTheme>(AlertTokenPatch {
    radius: Some(6.),
    default_padding: Some(AlertPadding::symmetric(8., 12.)),
    with_description_padding: Some(AlertPadding::all(16.)),
    with_description_icon_size: Some(32.),
});
```

Install the theme in a retained scope rather than recreating it during render.
Global aliases, component-local aliases and component algorithms determine the
cached colors, font, icon palette and motion. Default description titles inherit
the base line-height ratio at the enlarged title font size.
Native and browser captures are compared at matching device scales. Both renderers
resolve the nominal 1px border to one physical pixel (about 0.666667 logical pixels)
at 150% DPI, so the default title-only box measures about 39.333333 logical pixels.
The pinned GPUI text layout snaps unrounded line height to the device-pixel grid.
For example, the default enlarged title's 25.142857px leading becomes 25px at
100% and 200% DPI, and about 25.333333px at 150%. The bidi adapter uses the same
policy. Fractional leading and font rasterization can affect text baselines and
total heights, especially for multiple lines and compact themes.

## Accessibility and gallery

The root has native `Role::Alert` with assertive, atomic live announcements.
`accessibility_role(Role::Status)` chooses polite announcements; other roles
default to live Off. `accessibility_live` explicitly overrides the policy.
An Alert does not take focus when it appears. The close button supports pointer,
Enter, Space and native accessible activation, with a separate focus handle.
Focus moves to the next control only when the removed close button still owns it.

The close label resolves from an instance override, then the Alert scope, then the
existing locale's common Close message. Labels and descriptions supplied by the
application belong to its localization catalog. Use `accessibility_label` and
`accessibility_description` when custom content needs an explicit accessible name.
`automation_id` and `close_automation_id` are stable, untranslated identifiers.

The gallery's 13 cards preserve the applicable public reference order: Basic,
More types, Filled, Closable, Description, Icon, Banner, Smoothly Unmount,
Custom Icon, Custom action, Custom title alignment, Component Token and Custom
semantic dom styling. React ErrorBoundary and the external marquee example are
outside this native component.

## Focused verification

```powershell
cargo test --locked --offline --lib alert::
cargo test --locked --offline --example gallery alert_gallery
cargo clippy --locked --offline --lib --tests --example gallery --example alert_visual --bench alert -- -D warnings
./scripts/run-alert-perf.ps1
./scripts/test-alert-accessibility.ps1
```

The performance script requires the configured `windows-msvc-performance` build
tree with Release and benchmarks enabled. It measures cached style resolution,
builder construction, warm native drawing, and dismissal, checking zero settled
animation callbacks and one close completion. Offscreen CPU timings do not measure
GPU presentation or end-to-end input latency. The UI Automation script is opt-in
and requires an interactive Windows desktop.

The independent [browser fixtures](../fixtures/alert/README.md) execute the pinned
Alert component, Ant Design icons, original style generators and cssinjs serializer.
They cover four themes, both directions, rest/hover/focus, descriptions, variants,
custom content, actions, banners and token overrides. Cargo tests read committed
JSON and require no browser or JavaScript runtime. Deterministic native tests
cover activation, dismissal, configuration updates, focus and reduced motion.
Separate mounted browser traces capture description, title-only and explicitly
styled dismissal, including callback order and the actual CSS padding precedence.

```powershell
node scripts/generate_alert_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --util-package ../build/ant-design-gpui/alert-reference/vendor/package --browser 'C:/Program Files/Google/Chrome/Application/chrome.exe'
node scripts/capture_alert_lifecycle.mjs
node scripts/capture_alert_lifecycle.mjs --mode title --output fixtures/alert/lifecycle-title.json
node scripts/capture_alert_lifecycle.mjs --mode styled --output fixtures/alert/lifecycle-styled.json
./scripts/capture_alert_reference.ps1
cargo run --locked --offline --example alert_visual -- --output ../build/ant-design-gpui/alert-native
python scripts/compare_alert_visual.py --browser ../build/ant-design-gpui/alert-browser --native ../build/ant-design-gpui/alert-native --output ../build/ant-design-gpui/alert-comparison.json
cargo run --locked --offline --example gallery -- --component alert --headless --reduced-motion --capture ../build/ant-design-gpui/alert-gallery.png
```

Use existing packages as described in the fixture guide; `--util-package` is only
needed when the pinned util package is absent from the local pnpm store. Native
captures use a hidden GPU window; browser captures use isolated headless Chrome
profiles. Both cover 100%, 150% and 200% DPI. The image comparison reports signed
geometry differences and image error without alignment or font masking. Inspect
those measurements together because font shaping and antialiasing differ between
the native and browser renderers. Generated artifacts stay under ignored `build/`.
The paired matrix contains 56 images and 696 Alert rows: two content scenes across
four themes, both directions and three DPI scales, plus first-close hover/focus
captures at 100% in LTR for each theme. Browser style measurements and native
lifecycle tests provide separate evidence; image captures do not verify the
dismissal timeline.

## Recorded Windows validation

Validated on 2026-10-28 with Windows x64, Rust 1.98.1, the pinned GPUI revision,
and a Ryzen 9 5950X. The [verification record](../fixtures/alert/validation-windows.json)
contains commands, source and artifact hashes, accessibility results, and all
Release measurements.

- 35 Alert tests passed, including the independent browser styles, mounted motion
  traces, Unicode wrapping, truncation, cache invalidation and dismissal lifecycle.
- 16 configuration tests, four Alert gallery tests, two gallery CLI tests and five
  related pagination text tests passed. Targeted Clippy and Rust formatting checks passed.
- Windows UI Automation passed all 11 checks, including assertive live semantics,
  native focus/activation, independent actions, completion, and remounting.
- All 56 image pairs have matching physical dimensions at 100%, 150% and 200% DPI.
  Across 696 Alert rows, maximum control-size error is 0.328125 logical pixels;
  accumulated page-position error is 1.3125 pixels. Full, unaligned and unmasked
  image error averages 1.1731 per channel out of 255, with a maximum per-image mean
  of 2.1424. See the [visual record](../fixtures/alert/visual-validation-windows.json).

Selected warm CPU draw measurements, with descriptions and actions:

| Alerts | Direction | Median | p95 |
| --- | --- | ---: | ---: |
| 1 | LTR | 0.096 ms | 0.107 ms |
| 20 | LTR | 1.325 ms | 1.438 ms |
| 20 | RTL | 1.359 ms | 1.529 ms |
| 100 | LTR | 9.979 ms | 14.814 ms |
| 100 | RTL | 8.038 ms | 13.021 ms |

Cached style resolution measured 62.4 ns and builder construction 347.8 ns.
Every benchmark verified zero idle animation callbacks and exactly one completion
per dismissal. Repaired paragraph layouts retain at most four width variants per
mounted text node and invalidate on text, direction, typography, color or scale changes.

Remaining visual differences come from device-pixel leading, native SVG/font
rasterization and accumulated fractional spacing. The font probe confirmed Segoe UI
selection. These measurements include background compiler activity and measure CPU
layout/draw work; GPU presentation and end-to-end input latency were not measured.
Native macOS and Linux execution was not verified.

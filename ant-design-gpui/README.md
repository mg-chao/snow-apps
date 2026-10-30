# Ant Design GPUI

ColorPicker provides native solid-color and gradient editing with HEX/RGB/HSB
inputs, alpha, presets and custom panels. Open `--component color-picker` in the
gallery; see the [ColorPicker guide](docs/color-picker.md).

Image and PreviewGroup provide native thumbnails, loading/fallback/progress,
grouped fullscreen previews, keyboard navigation, zoom/pan/rotation/flips,
custom native content and scoped styling. Open `--component image` in the gallery;
see the [Image guide](docs/image.md) and [verification](docs/image-verification.md).

Apache-2.0 Ant Design controls built with GPUI. Button, Checkbox, Radio, Divider, Segmented, Tag, Alert, Descriptions, Result, Spin, Progress, Message and Modal use
native layout, focus, accessibility and motion, with retained configuration,
localization, complete themes and all 829 pinned icons.

The reference is Ant Design **6.6.7**, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. Windows native behavior is verified;
macOS/Linux APIs remain portable but require validation on those platforms.

## Component gallery

```powershell
cd ant-design-gpui
cargo run --locked --offline --example gallery
# Optional: -- --dark --compact --rtl --reduced-motion
# Reproduce a narrow layout: -- --window-size 640x720 --capture C:/temp/gallery.png
# Open Checkbox directly: -- --component checkbox
# Open Divider directly: -- --component divider
# Open Segmented directly: -- --component segmented
# Open Tag directly: -- --component tag
# Open Alert directly: -- --component alert
# Open Spin directly: -- --component spin
# Open Progress directly: -- --component progress
# Open Message directly: -- --component message
# Open Result directly: -- --component result
# Open Descriptions directly: -- --component descriptions
# Open Modal directly: -- --component modal
# Open Input directly: -- --component input
# Open InputNumber directly: -- --component input-number
# Open ColorPicker directly: -- --component color-picker
# Open Select directly: -- --component select
# Open Slider directly: -- --component slider
# Open Pagination directly: -- --component pagination
# Open Context Menu directly: -- --component context-menu
# Open Menu directly: -- --component menu
# Open Scroll Area directly: -- --component scroll-area
# Open Tabs directly: -- --component tabs
# Open Carousel directly: -- --component carousel
# Save a hidden native capture and exit: -- --component tabs --headless --capture C:/temp/tabs.png
```

The native gallery has a fixed component sidebar, direct example links and a
separately scrolling page. A persistent toolbar switches light/dark themes,
comfortable/compact density, preview direction and motion; its arrow returns to
the top. Display changes preserve active loading requests and scroll position
within the new page bounds. The sidebar becomes an icon rail in narrow windows.

Button's 15 public examples retain the reference checkout's order, labels and
descriptions. Nine previews are complete; six compact Planned cards explain the
capabilities needed for their full reference examples. Titles precede previews,
and cards use two columns only when the padded content has sufficient room.
The Color & Variant matrix follows the reference's responsive component sizing.
Tab/Shift+Tab navigate; Enter/Space activate controls.

Checkbox's seven public examples follow the reference order, with working
controlled state, disabled controls, groups, indeterminate Check all, native
grid layout and semantic styling. Select Checkbox in the sidebar or use
`--component checkbox`; Space toggles focused checkboxes.

Divider's nine examples cover horizontal and vertical separators, title placement,
spacing, plain text, solid/dashed/dotted lines, component tokens and semantic styling.
Select Divider in the sidebar or use `--component divider`.

Segmented's 13 public examples follow the reference order, with horizontal and
vertical groups, equal-width block layout, rounded shapes, disabled items,
controlled selection, custom content, dynamic options, sizes, icons and semantic
styling. Select Segmented in the sidebar or use `--component segmented`.

Tag's 12 examples follow the reference order, including preset and custom colors,
all three variants, cancelable closing, controlled checkable tags, single/multiple
groups, dynamic additions, entrance animation, native drag reordering, component
tokens, disabled controls and semantic styling. Select Tag or use `--component tag`.
The dynamic examples add sequential labels; text editing belongs to the host app.

Alert's 13 applicable public examples follow the reference order, with four statuses,
outlined/filled surfaces, descriptions, icons, banners, actions, measured dismissal,
component tokens and semantic styling. Select Alert or use `--component alert`.
See the [Alert guide](docs/alert.md) for ownership, accessibility and focused verification.

Result provides all seven statuses, the original exception illustrations, custom
content and actions, inherited semantic styling, and component tokens. Its gallery
follows the nine public reference examples and includes the token example. Select
Result or use `--component result`; see the [Result guide](docs/result.md).

Descriptions displays read-only fields in horizontal or vertical layouts, with
optional borders, responsive columns and spans, filled rows, custom content,
semantic styles and component tokens. Its gallery follows all eight public
reference examples and includes an additional token example. Select Descriptions
or use `--component descriptions`; size controls and Edit actions use native focus
and keyboard behavior. Responsive breakpoints follow the window width. See the
[Descriptions guide](docs/descriptions.md) for APIs, content ownership, styling,
accessibility, and native layout details.

Modal provides controlled dialogs and context-aware confirmation, information, success,
warning and error helpers. Its 17 reference examples cover asynchronous actions,
custom footers, loading, localization, drag positioning, responsive widths and styling.
Additional examples exercise nested overlays, tall content and retained native views.
See the [Modal guide](docs/modal.md) for ownership, lifecycle and verification.

Menu provides persistent horizontal, vertical and inline navigation, with collapsed
rails, responsive overflow, controlled selection/open keys and all 11 public
reference demos. See the [Menu guide](docs/menu.md) for integration and focused verification.

Context Menu and Scroll Area have retained GPUI controllers and dedicated gallery pages.
Context menus support nested and lazy branches, keyboard navigation, typeahead, checks, radio
groups, icons, shortcut hints and badges. Scroll areas use one native `ScrollHandle`
for wheel input, keyboard navigation, overlay thumb dragging and application updates.
See the [Context Menu and Scroll Area guide](docs/context-menu-scroll-area.md) for
integration, behavior and focused verification commands.

Tabs supports line, card and editable-card headers, all four logical placements,
RTL, retained lazy panes, controlled selection, virtualized overflow menus and
native keyboard navigation. The gallery contains the 16 public reference demos
and four debug scenarios, including custom search, sticky headers and drag reordering.
See the [Tabs guide](docs/tabs.md) for ownership, customization, measurements and
the focused `cargo test --locked --offline --lib tabs::` command.

Run only this example's offscreen regression tests with
`cargo test --locked --offline --example gallery`.

Carousel provides native retained slide views, scroll/fade effects, logical dot
placements, arrows, infinite navigation, dragging, adaptive sizing, and autoplay
with synchronized dot progress. Its gallery covers the six public reference
examples and component tokens. Select Carousel or use `--component carousel`;
see the [Carousel guide](docs/carousel.md) for ownership and focused verification.

Spin provides native four-dot loading, delayed activation, embedded content,
determinate and automatic progress, custom indicators, and fullscreen masks.
The gallery includes all nine public reference examples. See the
[Spin guide](docs/spin.md) and [verification report](docs/spin-verification.md)
for ownership, accessibility, visual comparisons, performance, and known limits.

## Native example and integration

Select and Slider have retained native controllers, typed values, virtualized
options, native text/IME integration and range editing. Their gallery pages cover
all 27 Select and 14 Slider public reference demos. See the
[Select and Slider guide](docs/select-slider.md) for APIs and focused verification.

Pagination uses a retained native controller and the existing Input/Select controls.
Its gallery page covers all 13 public reference demos, including controlled values,
simple mode, size selection, quick jumping and custom renderers. See the
[Pagination guide](docs/pagination.md) for APIs and reference details, and the
[verification report](docs/pagination-verification.md) for measurements and limits.

```powershell
cd ant-design-gpui
cargo run --locked --offline --example foundation
```

The example uses private inspection probes to demonstrate theme/density, locales,
RTL, icons, focus, availability and motion. Tab/Shift+Tab navigate; Enter/Space
activate. These probes are not exported library components.

Call `init(cx)` once per application (repeated calls are safe). Create a
`ConfigScope::new_window(cx)` per window and return a retained
`UiRoot::new(scope.clone(), content_view, window, cx)` from the window constructor.
The root installs namespaced traversal, native typography, text/background colors
and a non-tab-stop focus fallback. The example also calls
`window.activate_window()`; `App::activate` is a no-op on this pinned Windows backend.

GPUI 0.2.2 is pinned to `mg-chao/gpui` revision
`2c0bf70b54c018f99419f386d1cf1105dca57661`, matching `snow-draw-engine-gpui`.
Defaults are disabled and `gpu-extensions` enabled. Consumers select their own
backend; native platform dependencies are development-only here. Keep these
projects' GPUI pins synchronized.

## Input controls

`Input`, `TextArea`, `PasswordInput` and `SearchInput` share an `Entity<InputState>`.
Create the entity once with `cx.new(|cx| InputState::new(window, cx))`; render fresh
builders with stable IDs. `InputState::set_value` updates the view without emitting
a user change. Subscribe to `InputEvent` for changes, commits, search activation,
selection, composition and password visibility. `focus`, `blur`, `select` and
`select_all` provide programmatic control.

Common builders expose `size`, `variant`, `status`, `placeholder`, `allow_clear`,
`prefix`, `suffix`, `show_count`, `disabled` and `read_only`. `CountConfig` provides
an advisory maximum and a formatter. `InputState::set_max_length` sets a hard
editing limit; grapheme clusters are the default unit, with UTF-16 and custom
strategies available. Existing external values are not silently truncated.

`TextArea::auto_size(min_rows, max_rows)` follows content height within row bounds.
Overflow uses `ScrollArea` with an overlay scrollbar at the inner frame edge;
text retains its normal padding without an extra gutter. Wheel, thumb-drag and
caret scrolling share one position. Scrollbar dragging preserves
editor focus and selection; wheel events at the boundary reach enclosing scroll areas.
`PasswordInput` keeps visibility in the retained state; revealing a value preserves
selection and undo history, and never exposes it through clipboard or accessibility
text export. Use `set_password_visible` for external updates.
`SearchInput::enter_button(label)` adds a joined search action; `primary_action`
selects the icon-only equivalent, and `loading` suppresses activation. Outlined
inputs use a primary filled action; the other variants follow their corresponding
Ant Design text-button treatment. Search events identify Enter, action, clear and
programmatic sources. Affixed controls reserve the clear slot while it is hidden,
so availability and empty-value changes do not shift the text viewport.

Keyboard actions are public in `input::actions`, scoped to `AntDesignInput` while
the text editor holds focus. Applications may override their bindings. Ctrl/Cmd+Z
undoes; Ctrl/Cmd+Shift+Z redoes. Enter commits a single line; multiline Enter inserts
a newline and Ctrl/Cmd+Enter commits. Composition is a single undo transaction and
does not activate search.

The caret uses a 500 ms timer while the visible window and editor have focus.
Edits reset it; composition and reduced motion keep it steady. Dragging outside
the text viewport extends the selection with bounded, distance-based autoscroll.
Blur, disabling, hiding, release and unmount cancel the associated work.

`InputNumber` retains an `Entity<InputNumberState>` created with `new` or
`with_options(NumberOptions, window, cx)`. Render it with
`InputNumber::new(id, &state, cx)`. Use `NumberOptions::exact()` for
`NumericValue::Decimal(ExactDecimal)` or the default finite `f64` mode.
`None` represents an empty value. `ExactDecimal` parses signed decimal/exponent
strings and performs exact stepping and rounding without binary intermediates.
Its digit and scale limits bound work for extreme pasted exponents.

Numeric state separates the accepted value from the raw editing draft. Incomplete,
invalid and out-of-range drafts remain editable; Enter or blur clamps valid drafts,
applies precision and restores the previous accepted value for invalid drafts.
Set `change_on_blur: false` to defer this until Enter. Precision is deferred while
typing, and configured ranges take precedence when rounding would cross a boundary.
Standard-number ranges must contain a representable finite value.

`NumberOptions` supports minimum/maximum, step, precision, keyboard stepping,
opt-in focused wheel stepping, and parser/formatter callbacks. Formatters receive
the untouched input, accepted value, canonical display and `user_typing` flag.
Return `input` during typing to defer formatting. Presentation rewrites preserve
cursor position and amend the triggering undo transaction. Arrow keys and buttons
step once; Shift multiplies the step by ten. Held buttons repeat after 600 ms,
then every 200 ms, with cancellation on release, leave, blur, disable or unmount.

`InputNumberState::set_value` updates without user change events; equal owner echoes
preserve the current draft and composition. `value`, `text`, `draft_status`,
`set_options`, `focus`, `select`, `undo` and `redo` provide retained control.
Subscribe to `InputNumberEvent` for raw input, accepted changes, commits and steps
with their source. The builder supports the shared styling/affix/accessibility
options, default 90 px width, `block`, optional controls and custom step icons.

`OtpInput` uses an `Entity<OtpState>` with six cells by default. `OtpOptions`
configures length, an optional single-grapheme mask, and incoming-text formatter
and filter callbacks. Formatting and filtering precede the limit. A full-code
paste replaces the complete code from any cell; a partial paste starts at the
active cell. `value` returns concatenated characters and `cells` preserves empty
positions. `set_value` is silent; equal owner echoes preserve composition and
history. `OtpEvent` separates aggregate changes, completed codes, commits and
focus changes. `focus_cell`, `select_all`, `undo` and `redo` act on the retained
group. Arrow/Home/End navigation and a single Tab entry follow its active cell;
IME commits and multi-cell pastes form single undo transactions. Masked codes
remain protected from clipboard and accessibility text export. Cells shrink
together in narrow containers; `block` distributes the available width. Masked
glyphs inherit the surrounding OTP typography independently of input size tokens.

`InputGroup` joins inputs, numeric controls, buttons, composite search controls
and nested groups. Use `child`/`children`, `size`, `orientation`, `block`, and GPUI
flex/width builders. Explicit child size, availability and scope override inherited
values. Logical outer corners and overlapped borders follow direction and nesting;
raised focus/hover decoration preserves source-order Tab traversal and clipping.
Custom controls can implement `InputGroupItem` using `InputGroupContext` for
inherited configuration, border geometry and focus decoration.

The editor retains both caret positions at soft wraps. Pointer selection, vertical
navigation, Home/End and native candidate bounds use that affinity. Immutable
`value_snapshot` handles are shared between render and event consumers until the
text revision changes. Unchanged redraws reuse shaped paragraphs and geometry.
Navigation and native IME queries immediately after edits refresh the same cache
without waiting for a frame; the next draw reuses that geometry.
Frame colors and focus shadows, clear/password actions, and numeric handle width,
opacity, height and colors follow Ant Design's timing curves. Interrupted transitions
start from the painted value and preserve CSS reversal shortening. Reduced motion and hidden windows settle immediately;
settled controls schedule no animation frames. Foreground color changes reuse native
glyph shaping and selection geometry.

Run `cargo run --locked --offline --example input` for the current native verification
surface, or pass `-- --capture C:/temp/input.png` for a hidden-window capture.
The gallery has separate Input and InputNumber pages following the public
examples in the pinned Ant Design checkout, including their order, titles and
initial values:

```powershell
cargo run --locked --offline --example gallery -- --component input
cargo run --locked --offline --example gallery -- --component input-number
cargo test --locked --offline --example gallery -- input_gallery input_number_gallery --test-threads=1
```

Input has 18 reference cards (11 complete, six partial, one planned).
InputNumber has 14 (11 complete, two partial, one planned). Partial cards show
working rows and name their pending portions; the numeric-tooltip and spinner
cards are planned. Addon rows, the Input page's Select combination row, custom
password visibility icons, manual TextArea resizing, and independent internal-part
styling remain pending. The Select component itself is available on its own gallery page.
Styling previews demonstrate existing native root refinements and scoped tokens.
Named retained entities keep each row independent while switching pages, theme,
density, direction and motion. Standard count demos use UTF-16; custom emoji
counts use Unicode scalars. The existing standalone input app remains the native
verification surface.
Input uses the public GPUI text-layout and native-input APIs on every platform;
consumers do not need Cargo patches or vendored GPUI crates. Horizontal arrows,
word movement and selection collapse follow logical text order, following
[GPUI Kit's editing approach](https://github.com/longbridge/gpui-kit/blob/611b55e331170ca375214d580b6c5409db858f83/crates/base/src/input/base/movement.rs).
Grapheme-safe editing, UTF-16 IME conversion, undo/redo, soft-wrap navigation and
password protection remain supported. RTL control layout and alignment are retained,
but Arabic/Hebrew shaping, mixed-direction selection geometry and ligature caret
positions are limited by the pinned GPUI backend. Full browser-equivalent bidi
editing is not guaranteed. This removes the Input-specific extensions; the shared
`mg-chao/gpui` revision and `gpu-extensions` feature remain aligned with the drawing
engine. GPUI Kit is an architectural reference, not an added dependency.
Run `cargo run --locked --offline --example input -- --verify-navigation` for
native logical navigation, wrapping and IME-bound checks.
Verification evidence, measured performance and platform limitations are documented
in [VALIDATION.md](VALIDATION.md). Independent settled-style, composite, custom-token
and timed-motion measurements and regeneration steps are in
[fixtures/input](fixtures/input/README.md).

## Button

```rust
use ant_design_gpui::{Button, ConfigScope};
use ant_design_gpui::icons::IconName;

# fn save_button(scope: ConfigScope) -> impl gpui::IntoElement {
Button::new("save")
    .scope(scope)
    .label("Save")
    .primary()
    .icon(IconName::SaveOutlined)
    .on_click(|activation, window, cx| {
        // All activation paths share this callback.
    })
# }
```

Build Buttons during a view's render with stable, locally unique IDs. Internal
state retains focus and pending input across rerenders and is released on unmount.
Pass the window's `ConfigScope` with `.scope(...)`; omitting it uses application
defaults. Each Button observes its own scope, including inside cached parent views.
`.on_click(cx.listener(...))` works with a handler taking `&behavior::Activation`.

`ButtonType::{Default, Primary, Dashed, Text, Link}` supplies familiar appearances.
For independent choices, use `ButtonColor::{Default, Primary, Danger, Preset(...)}`
and `ButtonVariant::{Outlined, Dashed, Solid, Filled, Text, Link}`. Explicit color
and variant override their type defaults; `.variant(Solid)` alone implies primary.
`.danger(true)` selects danger unless color was explicit. The provider's input
`Variant` is independent of the Button variant.

Sizes inherit the provider's `Size` unless `.size(...)` overrides it. Shapes are
`Default`, `Square`, `Circle` and `Round`; upstream's square alias retains the
standard corner radius. `.block(true)` fills available width, and `.ghost(true)`
renders bordered appearances over their parent's background.

`.icon(...)` and `.icon_placement(Start | End)` follow logical RTL order. Empty
labels produce square icon-only controls; give these `.accessibility_label(...)`.
Plain two-character Chinese labels receive Ant Design's automatic space unless
an icon is present, the variant is Text/Link, or `.auto_insert_space(false)` is set.
Accessible names keep the original label. Applications localize labels and explicit
accessible descriptions through their own catalogs.

`.disabled(false)` overrides a disabled provider. Disabled controls leave the tab
order; `.loading(true)` blocks activation but retains focus. `.loading_delay(...)`
keeps the control ready until the delay expires, cancels when cleared or unmounted,
and does not restart on unrelated rerenders. `.loading_icon(...)` overrides the
default rotating loader. `.loading_icon_spin(true)` opts a custom icon into rotation;
`false` stops the default spinner. Static custom loaders do not schedule continuous frames.
Monochrome loaders rotate through the GPU atlas; colored icon rotation uses CPU
raster variants on the pinned GPUI backend.
Color transitions, loader expansion/collapse and click waves respect scoped and
application reduced motion. `.wave(false)` disables the click wave. Link controls
have no wave. Idle controls schedule no animation frames.

Pointer, Enter, Space and accessible invocation share availability guards. For
programmatic focus, pass a retained `behavior::focus_handle(cx)` using
`.focus_handle(&handle)`; externally supplied handles keep their tab settings.
`.href(...)` exposes native link semantics and opens through GPUI, with Enter
activation. `.automation_id(...)` supplies a stable native accessibility identifier.

Customize component tokens with `ComponentThemes::set::<ButtonTheme>(ButtonTokenPatch
{ ... })`. Component-local global overrides and algorithms affect the complete
control, including fonts, focus and motion. GPUI `Styled` refinements apply directly
to the native root, so relative widths, flex, margins and focus bounds agree.
`.icon_style(StyleRefinement)` and `.content_style(StyleRefinement)` customize the
icon slot and label independently while preserving the control's accessible name.

```powershell
cd ant-design-gpui
cargo run --locked --offline --example button
# Optional: --dark --compact --rtl --reduced-motion --capture C:/temp/button.png
cargo test --locked --offline --lib button::
./scripts/run-button-perf.ps1
```

The gallery contains interactive loading, theme, density, RTL, availability and
motion controls, all six variants and all 13 preset colors. The performance script
requires the repository's configured `windows-msvc-performance` Release build.
Independent [browser fixtures](fixtures/button/README.md) check 2,208 original
Ant Design computed styles across four themes and three interaction states.
`scripts/test-button-accessibility.ps1` provides opt-in Windows UI Automation checks.

## Checkbox

```rust
use ant_design_gpui::{Checkbox, ConfigScope};

# fn remember_checkbox(scope: ConfigScope) -> impl gpui::IntoElement {
Checkbox::new("remember")
    .scope(scope)
    .label("Remember me")
    .default_checked(true)
    .on_change(|change, window, cx| {
        // change.checked is the requested new value.
    })
# }
```

Use stable, locally unique IDs when building controls during render. An
uncontrolled Checkbox retains its own value; `.default_checked(...)` sets only
its initial value. `.checked(...)` supplies a controlled value: update application
state in `.on_change(cx.listener(...))` and rerender to accept the change.
`.indeterminate(true)` displays a mixed mark independently of the checked value,
which supports deriving a Check all control from the selected values.

The entire label toggles the control. Space and native accessibility actions use
the same guarded change handler. Disabled controls leave the tab order and
relinquish focus; `.disabled(false)` overrides a disabled provider. Supply
`.accessibility_label(...)` for an unlabeled Checkbox, `.automation_id(...)` for
native automation, and `.focus_handle(&handle)` for programmatic focus.

```rust
use ant_design_gpui::{ConfigScope, checkbox::{CheckboxGroup, CheckboxOption}};

# fn fruit_choices(scope: ConfigScope) -> impl gpui::IntoElement {
CheckboxGroup::new("fruit")
    .scope(scope)
    .accessibility_label("Fruit choices")
    .options([
        CheckboxOption::new("apple", "Apple", "Apple"),
        CheckboxOption::new("pear", "Pear", "Pear"),
        CheckboxOption::new("orange", "Orange", "Orange"),
    ])
    .default_value(["Apple"])
    .on_change(|values, window, cx| {
        // Values follow the current option order.
    })
# }
```

Groups accept typed values and explicit option IDs so focus remains stable when
options reorder. `.value(...)` controls selection; `.default_value(...)` initializes
retained selection. An option's explicit disabled setting overrides the group and
provider. Groups wrap with token spacing and support `.vertical(true)`; GPUI
`Styled` refinements also allow native grid layouts and full-width controls.

Checkbox follows the scope's theme, font, direction and motion policy. Ant Design
does not define Checkbox-specific component tokens; use
`ComponentThemeConfig::<CheckboxTheme>::default().with_tokens(...)` for component-local
global token overrides. Root `Styled`, `.icon_style(StyleRefinement)` and
`.label_style(StyleRefinement)` customize the semantic slots. The gallery shows
both static refinements and styles derived from controlled state.

```powershell
cargo run --locked --offline --example gallery -- --component checkbox
cargo test --locked --offline --lib checkbox::
cargo test --locked --offline --example gallery checkbox_
./scripts/run-checkbox-perf.ps1
```

Colors, ticks and click waves follow scoped motion and application reduced motion;
`.wave(false)` disables the wave. Idle controls schedule no animation frames. Tick
meshes are retained across redraws, color changes and scrolling, with no image assets.
Independent [browser fixtures](fixtures/checkbox/README.md) compare 576 original
Ant Design states across four themes, both directions, hover and keyboard focus.
`scripts/test-checkbox-accessibility.ps1` checks native Windows checkbox roles,
mixed state, toggling and keyboard interaction.

## Radio

```rust
use ant_design_gpui::{RadioGroup, RadioOption, ConfigScope};
use ant_design_gpui::radio::{RadioButtonStyle, RadioOptionType};

# fn choices(scope: ConfigScope) -> impl gpui::IntoElement {
RadioGroup::new("delivery")
    .scope(scope)
    .accessibility_label("Delivery method")
    .options([
        RadioOption::new("standard", "standard", "Standard"),
        RadioOption::new("express", "express", "Express"),
        RadioOption::new("pickup", "pickup", "Pickup").disabled(true),
    ])
    .default_value("standard")
    .option_type(RadioOptionType::Button)
    .button_style(RadioButtonStyle::Solid)
    .on_change(|value, window, cx| {
        // value is the requested selection; controlled owners apply it here.
    })
# }
```

`Radio::new(id).label("Radio")` creates a standalone circular control. Selecting
it sets its checked value to true; selecting it again never clears it or emits
another change. `.checked(bool)` controls it; `.default_checked(bool)` initializes
retained state once per mount. Enter does not select; Space, pointer clicks on
the complete label, and native accessibility actions use the same availability guard.

Groups accept typed `Clone + PartialEq` values and stable option IDs. `.value(T)`
controls selection; `.default_value(T)` initializes retained selection. The callback
receives `&T`. An option's `.on_change` runs before the group callback. Values
remain selected when temporarily removed from the options. No selection is valid;
use an `Option<T>` application value when a controlled group must explicitly clear it.

Tab enters the selected enabled option, or the first enabled option when there is
none, and the next Tab exits the group. Arrow keys wrap and skip disabled choices;
Left/Right mirror in RTL. Home/End select the first/last enabled option. Navigation
changes report `Activation::KeyboardNavigation` to option callbacks. Explicit
`.disabled(false)` on an option overrides group and provider disability, consistent
with this library's native option composition. Disabled controls relinquish focus.

`.vertical(true)` stacks options with token spacing; button groups join their
borders and stretch to equal widths. `.block(true)` fills available width and
shares it equally between horizontal options. Button sizes inherit `Size` from
the provider; circular radios retain their own token-defined dimensions.

Radio follows scoped theme, direction, fonts and motion. Register `RadioTokenPatch`
with `RadioTheme` for all upstream component tokens, including circle/dot dimensions,
button palette/padding and wrapper spacing. `Styled` refines the native root;
`.icon_style(...)` and `.label_style(...)` refine a standalone circle and label.
Use `.accessibility_label(...)` for unlabeled controls and `.automation_id(...)`
for native automation. Supply a retained `.focus_handle(...)` for programmatic focus.

Circular controls use GPU quads; their dots use centered vector paths so they stay
aligned at fractional display scales. Neither requires image assets.
Connected button borders and keyboard outlines paint after their siblings within
the group's normal clipped pass. The ring and dot use their separate Ant Design
transition timings; live dot-size changes animate with the dot. Animations respect
reduced motion and schedule no frames while idle; `.wave(false)` disables click waves.

```powershell
cargo run --locked --offline --example gallery -- --component radio
cargo test --locked --offline --lib radio::
cargo test --locked --offline --example gallery radio_gallery
./scripts/run-radio-perf.ps1
./scripts/test-radio-accessibility.ps1
```

The gallery includes the eleven public Ant Design Radio examples and a component
token example. Independent [browser fixtures](fixtures/radio/README.md) compare
3,200 original controls across themes, interaction states, RTL, custom tokens,
wireframe, all button sizes, and connected layouts.

## Divider

```rust
use ant_design_gpui::{Divider, Size};
use ant_design_gpui::divider::{DividerTitlePlacement, DividerVariant};
use gpui::px;

let section = Divider::new("details")
    .label("Details")
    .title_placement(DividerTitlePlacement::Start);
let subtle = Divider::new("subsection")
    .label("More information").plain(true)
    .variant(DividerVariant::Dashed).size(Size::Small);
let inline = Divider::new("actions-separator").vertical(true);
let edge_title = Divider::new("edge-title")
    .label("Settings")
    .title_placement(DividerTitlePlacement::End)
    .title_margin(px(0.));
# let _ = (section, subtle, inline, edge_title);
```

Pass `.scope(scope.clone())` to inherit a window's theme, direction and explicitly
configured size. Unset horizontal spacing is 24px without a title and 16px with
one; small/medium spacing is 8px/16px. Large preserves the unset spacing. Theme
algorithms derive these values from the effective aliases. Vertical dividers use
0.9em height and 8px inline margins by default; place them in an `items_center()`
row alongside text or actions. Vertical dividers ignore titles and size.

`Start` and `End` mirror in RTL; `Left` and `Right` are physical placements.
`.title_margin(...)` replaces the short rail and outer padding for edge titles.
`.child(...)` accepts any GPUI title element; `.label(...)` accepts text, including
`"0"`. The last title setter wins. Titles remain on one line; narrow layouts let
the rails collapse before the title overflows, matching Ant Design.

Native `Styled` refinements apply to the root. `.rail_style(...)` and
`.content_style(...)` refine the two semantic slots. Border color/width style
the rail stroke. Register `DividerTokenPatch` through `DividerTheme` for scoped
title padding, edge rail proportions and vertical margins. Padding accepts
pixels/rems or font multiples (`relative(1.)` means 1em).

Dividers expose native separator roles and orientation without a tab stop.
Text titles supply their accessible name; use `.accessibility_label(...)` for
custom titles when necessary. They own only a scope subscription and two bounded
paint caches. Solid rails use one quad; dashed/dotted rails reuse tessellation
across redraws, scrolling and color changes, with no per-mark layout children.
Idle dividers schedule zero animation frames.

Run `cargo test --locked --offline --lib divider::` and
`cargo test --locked --offline --example gallery divider` for focused coverage.
Independent browser references and regeneration instructions are in
[`fixtures/divider/README.md`](fixtures/divider/README.md).
Run `scripts/run-divider-perf.ps1` for optimized Release measurements under
`windows-msvc-performance`.

## Segmented

```rust
use ant_design_gpui::{ConfigScope, Segmented, SegmentedOption};

# fn period_picker(scope: ConfigScope) -> impl gpui::IntoElement {
Segmented::new("period")
    .scope(scope)
    .accessibility_label("Period")
    .options([
        SegmentedOption::new("daily", "daily", "Daily"),
        SegmentedOption::new("weekly", "weekly", "Weekly"),
        SegmentedOption::new("monthly", "monthly", "Monthly"),
    ])
    .default_value("weekly")
    .on_change(|value, window, cx| {
        // `value` is the requested selection.
    })
# }
```

Build controls during render with stable group and option IDs. Option values can
be any supported cloneable, comparable type; IDs keep focus stable when options
reorder. An uncontrolled Segmented initially selects its first option, or the
explicit `.default_value(...)`. Use `.value(...)` with `.on_change(cx.listener(...))`
to keep selection in the containing view. Removed values are retained, so restoring
an option restores its selection. Mounting with no options starts without a selection.

Tab enters a radio group at its selected enabled option. Arrow keys select and
focus adjacent enabled options, wrapping inside the group; horizontal navigation
follows RTL. Space selects the focused option. Pointer, keyboard and native
accessibility activation use the same selection callback. A disabled group or
option cannot be selected; `.disabled(false)` on the group overrides a disabled
provider.

`.size(Size::Small | Medium | Large)` overrides the scope's size.
`.vertical(true)` lays out options in a column; `.block(true)` fills the available
width with equal-width horizontal options. `.shape(SegmentedShape::Round)` uses
rounded tracks and items. Explicit `.orientation(SegmentedOrientation::Horizontal | Vertical)`
takes priority over `.vertical(...)`. Labels truncate to fit narrow horizontal layouts.

Options accept `.icon(...)`, `.child(...)` for custom native content,
`.tooltip(...)`, `.accessibility_label(...)` and `.automation_id(...)`.
The label passed to `SegmentedOption::new` remains the accessible fallback when
custom content replaces the visible label. Give icon-only options explicit names.
GPUI `Styled` refinements customize the root; `.item_style(...)`, `.label_style(...)`
and `.icon_style(...)` customize semantic slots. Component token overrides use
`SegmentedTheme` and `SegmentedTokenPatch`.

```powershell
cargo run --locked --offline --example gallery -- --component segmented
cargo test --locked --offline --lib segmented::
cargo test --locked --offline --example gallery segmented_
./scripts/run-segmented-perf.ps1
./scripts/test-segmented-accessibility.ps1
```

The gallery includes all 13 public reference examples. Independent
[browser fixtures](fixtures/segmented/README.md) compare 576 original states
across four themes, both directions, sizes, shapes and interaction states.
Selection motion follows actual native item bounds, including custom content and
unequal widths, and remains continuous when interrupted. Reduced motion snaps
directly to the final surface. Idle controls schedule no animation frames.
Fonts, icon palettes and shadows are cached with component tokens; mounted option
reconciliation is linear in the number of options.

## Tag

```rust
use ant_design_gpui::{ConfigScope, Tag};
use ant_design_gpui::tag::{TagColor, TagVariant};
use ant_design_gpui::icons::IconName;

# fn status_tag(scope: ConfigScope) -> impl gpui::IntoElement {
Tag::new("status")
    .scope(scope)
    .label("Ready")
    .color(TagColor::Success)
    .variant(TagVariant::Outlined)
    .icon(IconName::CheckCircleOutlined)
    .closable(true)
    .on_close(|event, window, cx| {
        // event.prevent_default() keeps the tag visible.
    })
# }
```

Build tags with stable, locally unique IDs. Closing hides a retained tag until it
unmounts; `.on_close(...)` runs before hiding and can cancel with
`event.prevent_default()`. The close control has its own tab stop and localized
accessible label, and supports pointer, Enter, Space and native accessible actions.
Closing a link tag does not follow the link or activate its outer click handler.
Use `.href(...)` for link semantics or `.on_click(...)` for an action.

`TagColor` covers all `PresetColor` families, Success, Processing, Warning, Error,
Default and custom `Color` values. `TagVariant::{Filled, Solid, Outlined}` follows
Ant Design 6, including HSL tints for custom filled/outlined colors. Explicit
`.disabled(false)` overrides a disabled provider. Disabled tags block all actions.
Tag geometry follows its component tokens independently of provider input size.

`CheckableTag::new(id).checked(value)` is controlled: accept the proposed
`TagChange.checked` in `.on_change(...)` and rerender. Space, pointer and accessible
actions propose the inverse of the current value. `CheckableTagGroup` accepts
typed `TagOption::new(id, value, label)` entries, single selection by default and
`.multiple(true)` for multiple choices. `.value(...)` controls a group;
`.default_value(...)` initializes retained selection. Change callbacks receive
the selected values as a slice; a single group contains zero or one value.

Use native `Styled` refinements on the root, and `.icon_style(...)`,
`.content_style(...)`, and `.close_style(...)` for semantic slots. A scoped
`TagTheme`/`TagTokenPatch` customizes `default_bg`, `default_color` and
`solid_text_color`. Colors, fonts and motion follow the effective component theme;
idle tags request no animation frames.

```powershell
cargo run --locked --offline --example gallery -- --component tag
cargo test --locked --offline --lib tag::
cargo test --locked --offline --example gallery tag_
./scripts/run-tag-perf.ps1
./scripts/test-tag-accessibility.ps1
```

## Alert

```rust
use ant_design_gpui::{Alert, AlertType, ConfigScope};

# fn notice(scope: ConfigScope) -> impl gpui::IntoElement {
Alert::new("upload-notice")
    .scope(scope)
    .alert_type(AlertType::Success)
    .title("Upload complete")
    .description("The document is ready to share.")
    .show_icon(true)
    .closable(true)
    .on_close(|_activation, _window, _cx| {})
    .after_close(|_window, _cx| {})
# }
```

Build with a stable ID. Closing persists until remount: `on_close` reports accepted
dismissal immediately and cannot cancel it; `after_close` runs once after the
measured collapse and fade. Parent unmount cancels completion. Reduced motion
finishes immediately with the same callback order, and idle Alerts schedule no frames.

Use text or native content for title, description, icon, action and close slots.
`AlertVariant::{Outlined, Filled}`, banner defaults, RTL, component tokens and
inherited `ConfigPatch::alert` defaults follow the reference. `AlertStyles` and
style resolvers customize semantic slots; GPUI `Styled` customizes the root.
`close_disabled` is instance-only. The root announces as a native Alert without
taking focus; the close button uses the existing localized Close label and supports
pointer, Enter, Space and accessible activation.

```powershell
cargo run --locked --offline --example gallery -- --component alert
cargo test --locked --offline --lib alert::
cargo test --locked --offline --example gallery alert_gallery
./scripts/run-alert-perf.ps1
./scripts/test-alert-accessibility.ps1
```

See the [Alert guide](docs/alert.md) and
[independent browser fixtures](fixtures/alert/README.md) for API details,
source provenance and native/browser visual comparison commands.

## Retained configuration

Scopes are explicit, cheaply cloned handles for views, callbacks and overlays.
Configuration-dependent views retain `scope.observe(cx)`'s subscription. Rendering
reads `scope.snapshot(cx)` without regenerating tokens. GPUI coalesces inherited
updates at the end of the current application update.

```rust
use ant_design_gpui::{ConfigPatch, ConfigScope, Direction, Size};
use ant_design_gpui::theme::{AlgorithmChain, Color, SeedTokenPatch, ThemeConfig};

# fn configure(cx: &mut gpui::App) -> Result<(), Box<dyn std::error::Error>> {
let window = ConfigScope::new_window(cx);
let local = window.child(ConfigPatch {
    direction: Some(Direction::Rtl),
    size: Some(Size::Small),
    theme: Some(ThemeConfig {
        seed: SeedTokenPatch {
            color_primary: Some(Color::rgb(0x722ed1)),
            ..Default::default()
        },
        algorithms: Some(AlgorithmChain::dark_compact()),
        ..Default::default()
    }),
    ..Default::default()
}, cx)?;
let accent = local.snapshot(cx).theme.tokens.color_primary;
# let _ = accent;
# Ok(())
# }
```

`ConfigScope::application(cx)` controls shared defaults. Scopes inherit unset
fields; explicit `false` overrides inherited `true`. `ThemeConfig::inherit=false`
resets theme inheritance. Locale selection replaces the base catalog before local
message overrides; direction is independent of language.

`set_patch` replaces local overrides; `update_patch` edits and resolves once.
Invalid updates leave patch/snapshot unchanged. If a parent change invalidates a
custom child algorithm, the child keeps its last good snapshot and exposes
`last_error` until recovery. Effective no-op updates preserve snapshot identity
without notifying. Weak observers avoid cycles; children release obsolete themes.

Snapshots cache fonts and icon palettes. `availability(local_disabled, busy)`,
`focus_ring()` and `motion_policy(cx)` give controls consistent defaults.

## Complete native tokens

`SeedTokens`, `MapTokens`, `AliasTokens` and their optional-field patches come
from one inventory. Colors use sRGB; dimensions are logical pixels; durations
are `Duration`. Font stacks, easing, decorations and shadows have native types.
`Color` converts directly to GPUI `Rgba`, `Hsla` and `Fill`.

`ThemeConfig::try_resolve` validates customization. Resolution merges seeds,
runs ordered default/dark/compact algorithms, applies map overrides, derives
aliases and reapplies explicit final overrides. Custom `MappingAlgorithm`
implementations must be pure; custom steps compare by `Arc` identity. The HSV
palette algorithm and rounding follow upstream. Typed preset/tone indexing
unifies legacy spellings such as `blue-1` and `blue1`.

Controls implement `ComponentTheme` with associated token/patch types,
default derivation, merging and application. Register through
`ComponentThemes::set` or `set_theme`; resolve with
`ResolvedTheme::component::<Marker>()`. Results are cached per type/snapshot.
Component-specific global overrides and algorithm chains are supported.

## Locales and icons

`Locale::{en_us,zh_cn,zh_tw}` provide complete typed common catalogs. `MessageKey`
is independent of translated text. `Locale::custom(id, patch)` falls back to
English per field; `LocalePatch` provides inherited overrides. Component catalogs
will accompany their controls. Button's name comes from its content or explicit
accessible label, rather than a generic icon/loading announcement.

`IconName` contains 829 variants. `IconRef` supports built-ins, static custom
descriptors outside the `antd/` namespace, and the host's existing asset paths.
No application asset source replacement is required.

```rust
use ant_design_gpui::icons::{IconName, icon};
use gpui::{Styled, px};
let search = icon(IconName::SearchOutlined).size(px(16.));
// In a view, also pass .palette(scope.snapshot(cx).icon_palette).
# let _ = search;
```

Icons are decorative; the parent control owns accessible meaning. Artwork only
mirrors when requested. Monochrome rotation uses atlas transforms; use
`LoadingOutlined` for animated loading. Static full-color transforms are cached.
Two-tone output preserves source layering/effects; its secondary default is Ant's
light palette tint of the theme primary color.

The app-owned `IconRenderer` defaults to 256 parsed entries, 512 raster entries,
16 MiB raster data and 1 MiB per raster. Limits cover cache-owned CPU data.
Active/cached GPUI scenes retain separate image leases so eviction cannot
invalidate a replayed scene. Resident-image statistics expose this working set;
weak resident lookup avoids rerasterizing visible images when CPU limits are
smaller than that set. Final lease release reclaims matching atlas entries.
Default trim/clear helpers dispose unleased images after painting; standalone
eviction returns retired images to the owner. Clear caches after replacing a host
asset in place; clear refreshes windows once to update cached scenes safely.
Warm rendering reuses immutable content identities and parsed/rasterized data.

## Focus, accessibility and motion

Retain one `ActivationGuard`, focus handle and blur subscription per actionable
control. Update availability immediately when state changes. Apply the guard to
the identified element, then the focus ring, then `Accessibility`. Disabled
controls relinquish focus and traversal; busy controls retain focus and block
activation. Pointer, keyboard and accessible actions share the guarded dispatch.

`Accessibility` preserves the underlying element's identity and lifecycle while
adding disabled/busy/invalid/read-only/hidden metadata. Existing GPUI APIs supply
values, selected/checked/expanded state and other semantics. Icon-only controls
need purpose-specific labels. Focus outlines have a transparent 1px gap and do
not alter layout/shadows. Programmatic focus preserves GPUI's input modality.

`MotionPolicy` adapts token durations/easing to GPUI animations. Zero-duration or
disabled motion is static; reduced-motion loops schedule no frames.
`ScalarTransition` provides continuous retargeting with caller-owned time and no
independent timer. Hosts propagate OS preferences with `App::set_reduce_motion`;
this GPUI revision does not do so automatically. Scope overrides never change
that application preference.

## Focused verification

Use Rust 1.98.1 and the VS 2026 x64 environment on Windows:

```powershell
cargo check --locked --offline --lib --examples --benches
cargo fmt --check
cargo clippy --locked --offline --lib --tests --examples --benches -- -D warnings
cargo test --locked --offline --lib
python scripts/generate_icons.py --check
./scripts/test-native-accessibility.ps1
./scripts/run-foundation-perf.ps1
```

These cover only the foundation crate. Native UIA verification is opt-in and
briefly opens an interactive window, checking actual names, roles, focus,
availability and invocation. Offscreen checks cover scope updates, traversal,
press cancellation, focus geometry, animation scheduling and icon atlas reuse.

Static fixtures compare the complete seed/map/alias inventory against execution
of pinned upstream TypeScript, colors 8.0.1 and fast-color 3.0.1. Regeneration and
provenance are documented in `fixtures/theme/`. Cargo needs no Node/Python,
external checkout or network for fixture/icon generation.

The performance script verifies the `windows-msvc-performance` tree, then runs
Cargo's optimized Release benchmark beneath it. Measurements cover cold themes,
deep scope updates, no-op/read paths, warm two-tone rendering and cache churn.
Warm paths assert unchanged parse/raster misses. Timings are diagnostic rather
than hardware-independent CI thresholds.

See [LICENSE](LICENSE), [COPYRIGHT](COPYRIGHT) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), plus the recorded
[validation results](VALIDATION.md). Generated builds, measurements
and license bundles stay under the repository's ignored `build/` directory.

## Switch

```rust
use ant_design_gpui::{ConfigScope, Switch};

# fn notification_switch(scope: ConfigScope) -> impl gpui::IntoElement {
Switch::new("notifications")
    .scope(scope)
    .accessibility_label("Notifications")
    .default_checked(true)
    .checked_label("On")
    .unchecked_label("Off")
    .on_change(|change, window, cx| {
        // change.checked is the requested value; change.activation identifies the input.
    })
# }
```

Build switches with stable, locally unique IDs during render. Uncontrolled switches
retain their value; `.default_checked(...)` initializes it once. `.checked(value)`
is controlled: accept `SwitchChange.checked` in `.on_change(cx.listener(...))` by
updating application state and notifying the view. `.on_click(...)` runs after
the change callback for pointer, Space, Enter and accessible invocation. Left and
Right request off and on respectively, including in RTL, and emit only a change
when the requested value differs.

`.checked_label(...)` and `.unchecked_label(...)` supply text; `.checked_content(...)`
and `.unchecked_content(...)` accept decorative native elements such as icons.
Both slots are measured to keep width stable when toggling. Keep custom content
noninteractive and give every switch a stable `.accessibility_label(...)` that
describes the setting. The root exposes native switch and checked semantics;
`.automation_id(...)`, `.accessibility_description(...)` and `.focus_handle(...)`
support automation, context and programmatic focus. Localize labels in the host app.

Sizes inherit the scope: `Size::Small` selects the small track, while Medium and
Large use the normal track. `.disabled(false)` overrides a disabled provider.
`.loading(true)` shows the spinner and disables activation, focus and traversal,
matching Ant Design. Handle motion, track colors, loading rotation and waves
respect scoped and application reduced motion; `.wave(false)` disables the wave.
Idle switches request no animation frames.

Native `Styled` refinements apply to the root; `.content_style(StyleRefinement)`
and `.indicator_style(StyleRefinement)` customize the semantic slots. Register
`SwitchTokenPatch` with `ComponentThemes::set::<SwitchTheme>(...)` to customize
track height/minimum width/padding, handle size/background/shadow and content
margins, including small-size values. Component-local aliases and algorithms
also control colors, fonts, focus and motion.

```powershell
cargo run --locked --offline --example gallery -- --component switch
cargo test --locked --offline --lib switch::
cargo test --locked --offline --example gallery switch_
./scripts/run-switch-perf.ps1
```

The focused Release benchmark measures cached styles, builders and warm drawing
or rerendering of 100 plain, text-labeled and icon switches. Each draw verifies
that resting switches schedule no animation frames with motion enabled.

## Popups

Tooltip, Popover, and Popconfirm share an anchored popup foundation with scoped
configuration, native content factories, keyboard interaction, and guarded async
confirmation. `UiRoot` supplies their window-local host. See the
[popup guide](docs/popups.md), [verification report](docs/popup-system.md), and the
gallery's `--component tooltip`, `popover`, and `popconfirm` pages.

## Message

Message provides window-scoped feedback with five status types, native rich content,
timed or persistent notices, hover pausing, keyed updates, count limits and expandable
stacks. `UiRoot` supplies a default host; use `Message::for_window(window, cx)` inside
callbacks, or retain a `MessageState` and mount `MessageHost` for a separate scope.
Handles expose programmatic closing and awaitable completion without retaining the
window. Component tokens and six semantic style slots follow the effective theme.

The gallery includes all nine applicable public reference examples and a component
token example. See the [Message guide](docs/message.md) and
[verification guide](docs/message-verification.md) for ownership, completion timing,
upstream comparisons and platform limits.

```powershell
cargo run --locked --offline --example gallery -- --component message
cargo test --locked --offline --lib message:: -- --test-threads=1
cargo test --locked --offline --example gallery message_gallery
./scripts/test-message-accessibility.ps1
./scripts/run-message-perf.ps1
```

## Progress

Progress provides controlled native line, circle and dashboard indicators with
success segments, steps, gradients, custom content and logical label placement.
Its gallery contains all sixteen public reference examples and a token example.
See the [API guide](docs/progress.md) and [verification report](docs/progress-verification.md).

## Notification

Notification provides six window placements, status and custom icons, native
content and actions, timed progress, keyed updates, count limits, expandable
stacks, and scoped semantic styles. `UiRoot` supplies the default window service;
independent retained controllers use `NotificationHost`. See the
[Notification guide](docs/notification.md) and
[verification guide](docs/notification-verification.md).

```powershell
cargo run --locked --offline --example gallery -- --component notification
cargo test --locked --offline --lib notification:: -- --test-threads=1
cargo test --locked --offline --example gallery notification_gallery
./scripts/test-notification-accessibility.ps1
./scripts/run-notification-perf.ps1
```

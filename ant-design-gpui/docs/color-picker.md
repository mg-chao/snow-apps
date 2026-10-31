# ColorPicker

ColorPicker provides native solid-color and linear-gradient editing. Its visual
reference is Ant Design 6.6.7, commit
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`.

Create one retained controller per mounted picker and rebuild its lightweight
builder during render:

```rust,ignore
let picker = cx.new(|cx| ColorPickerState::new(window, cx));

// In render:
ColorPicker::new("accent", &picker)
    .scope(scope.clone())
    .default_value(PickerColor::parse("#1677ff").unwrap())
    .show_text(true)
    .allow_clear(true)
    .on_change(|change, _, _| {
        println!("{}", change.css);
    });
```

Use the library's `UiRoot` so the shared popup host, native typography and keyboard
traversal are installed. Supply the same `ConfigScope` to the picker and its
containing view.

## Values and ownership

`PickerColor` retains floating-point hue, saturation, brightness and alpha. Hue
uses degrees; saturation, brightness and alpha use 0–1. RGB constructors use
0–255. Conversion to byte RGB happens only for exported RGB/HEX values; a gray or
black edit therefore retains its selected hue.

`ColorPickerValue` distinguishes `Cleared`, `Single(PickerColor)` and
`Gradient(Arc<[ColorStop]>)`. Cleared is different from a selected color whose
alpha is zero. Use `ColorPickerValue::gradient` to validate and stably sort stop
percentages. Empty gradients represent clear; a supplied one-stop gradient is
accepted. Interactive deletion consistently keeps at least two stops.

The parser supports 3/4/6/8-digit HEX with an optional `#`, named CSS colors,
`transparent`, RGB(A), HSL(A), and HSB/HSV(A), including percentage channels and
space/slash functional notation. Unsupported or malformed strings return an
error. CSS variables and wide-gamut color functions are not supported.

`value` enables controlled proposals. Subscribe to `on_change` to accept changes
continuously, or `on_change_complete` to accept only the final gesture value.
The panel previews a controlled drag while the trigger retains the accepted
value. After completion it returns to the supplied value if the owner rejects
the proposal. Programmatic state setters do not emit user events.

`Change` is emitted during pointer or keyboard edits; `ChangeComplete` is emitted
once on release. Discrete input, preset, mode and clear edits emit both. Disabling,
closing, deactivating or unmounting cancels an active gesture without a late
completion. Callback dispatch leaves the controller borrow before calling
consumer code, allowing the callback to update the same entity.

`default_value`, `default_format` and `default_open` initialize once. `format`
and `open` are independently controlled. Default configuration is a cleared
color, HEX format, single mode, closed popup, click trigger, bottom-left placement
and a visible arrow.

## Gradients and customization

Enable `.modes([ColorPickerMode::Single, ColorPickerMode::Gradient])` for the mode
selector, or `.mode(ColorPickerMode::Gradient)` for gradient-only editing. Rail
clicks insert interpolated stops. Handles retain identity when crossing; dragging
away or pressing Delete/Backspace removes a stop. Switching to single and back
restores the previous gradient until the single color is edited.

`disabled_alpha(true)` hides alpha controls. Supplied values retain their display
alpha; every emitted non-cleared value becomes opaque, including every gradient
stop. This deliberately corrects the upstream behavior that can flatten a
gradient during opacity normalization.

`ColorPickerPreset::new(key, label, colors)` creates a keyed collapsible group;
groups default to expanded. Use `default_open(false)` for a collapsed group and
`label_content` for native label content.
`try_new` returns validation errors for untrusted preset data; `new` expects valid
colors. Both normalize empty gradients to clear and stably sort gradient stops.
Raw values assigned through public preset fields are checked again before the
component updates its controller.

Use `child` for a custom trigger and `text_render` for native trigger text.
`panel_render` receives fresh `ColorPickerPanelParts` factories:
`panel(window, cx)`, `picker(window, cx)` and `presets(window, cx)`. These share the
same retained controller and can be rearranged into a custom native layout.
`ColorPickerPanel` embeds the full editor without a popup.

`ColorPickerStyles` exposes root, body, content, description, popup, palette,
slider, input and preset refinements. `styles_fn` resolves refinements from the
current state. Scoped defaults use `ColorPickerConfigPatch`; component tokens use
`ColorPickerTheme` and `ColorPickerTokenPatch`. Size, disabled state, direction,
theme, locale and motion inherit through `ConfigScope`.

The reference semantic `body` and `content` slots style the trigger's outer swatch
and inner color fill; `description` styles trigger text. They do not resize the
editing panel. Use ordinary `Styled` methods to style a standalone
`ColorPickerPanel`, or rearrange panel parts for a custom popup layout.

## Keyboard and accessibility

Enter/Space opens the focused trigger. Escape dismisses the popup and restores
focus. Native editors support selection, clipboard, undo and IME. Format selection,
preset groups, preset swatches and mode selection participate in keyboard
navigation.

The palette supports horizontal saturation and vertical brightness changes.
Named saturation, brightness, hue, alpha and gradient-stop range controls expose
native numeric accessibility actions. Arrow keys step; Home/End reach bounds;
Page Up/Down use larger steps. Shift increases the step size. Color coordinate
axes remain left-to-right in RTL, matching the reference, while surrounding
layout follows the configured direction.

## Focused verification

```powershell
cargo test --locked --offline --lib color_picker
cargo test --locked --offline --example gallery color_picker
cargo run --locked --offline --example gallery -- --component color-picker
```

The independent browser source, capture scenarios, comparator and package notices
live under `fixtures/color_picker` and `scripts/*color_picker*`. Generated PNGs
and executables remain in the ignored build tree. Release measurements use only
the repository's `windows-msvc-performance` preset and the dedicated
`run-color-picker-perf.ps1` script. No full test suite is needed.

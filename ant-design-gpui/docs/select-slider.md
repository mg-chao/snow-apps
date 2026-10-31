# Select and Slider

These controls use retained GPUI entities for interaction and fresh `RenderOnce`
builders for presentation. Call `ant_design_gpui::init(cx)` once and mount your
view beneath `UiRoot`, which owns popup layers and traversal. Reuse the same
`ConfigScope`, entity, and element ID while a control remains mounted.

## Select

Create immutable options and the controller outside `render`:

```rust,ignore
let options = SelectOptions::new([
    SelectOption::new("jack", "jack".to_owned(), "Jack"),
    SelectOption::new("lucy", "lucy".to_owned(), "Lucy"),
]).unwrap();
let selection = cx.new(|cx| SelectState::new(options, window, cx));

// In render:
Select::new("person", &selection)
    .scope(scope.clone())
    .placeholder("Choose a person")
    .accessibility_label("Person")
    .allow_clear(true)
    .w(px(240.))
```

Values can be domain types implementing `Clone + Eq + Hash`. `SelectOption::new`
also uses `Display` for value-based searching; `with_value_text` accepts that text
explicitly. IDs and values must be unique within a collection. Labels need not be
unique. `SelectOptions::from_entries` accepts options and `SelectGroup`s. Cloning
the collection shares its rows, search fields, and lookup tables.

`SelectSettings` chooses `Single`, `Multiple`, or `Tags`, a selection limit,
hide-selected behavior, initial active-option behavior, and token separators.
For editable string tags, use `SelectState::<String>::tags`. For validated domain
tags, set a `SelectTagFactory` returning an option or `None`. A `SelectTokenizer`
can handle quoted fields or application-specific separators. Returning an
unchanged single token preserves the editor query.

`SelectSearch` retains search policy: enable editing, match values, labels or
additional named fields, supply a filter/sort, disable local filtering for remote
data, and choose whether selecting clears the query. Default search is enabled
for multiple/tags and disabled for single selection. The default filter searches
value text; `SelectSearch::label()` searches labels. Additional fields use OR
matching. Reuse policies containing closures so cached results remain valid.
`filter_entries` and `sort_entries` also receive group headers as `SelectRow`s.
A matching group includes its children; sorting keeps each group together and
sorts its children separately. These callbacks take precedence over option-only
callbacks. Custom sorting also places user-created tags among supplied options.

Use `set_values`, `set_value`, `set_options`, `set_search_value`, and `set_open`
for silent external changes. `set_value` accepts labeled values, preserving a
selected label while remote options are unloaded. Current option labels replace
cached labels when the same value reappears. `focus`, `blur`, and `scroll_to` use
the retained native editor and option identity.

Subscribe to `SelectEvent` or use `on_event`. User changes carry both values and
labels, plus their source. `Change` precedes `Select`/`Deselect`; clearing emits
`Clear` and then `Change`. Search, open changes, active-option changes, focus and
blur are separate events. Matching the original, separator tokenization reports
all resulting selected values. In multiple mode, label tokenization can resolve a
disabled option; direct selection and removal still respect disabled options.

`value`, `open`, and `search_value` builders independently enable controlled
state. Accept proposals by updating the owning state and passing the new value
on the next render. Rejected proposals restore the accepted value/query. Avoid
constructing large controlled value vectors on unrelated redraws; a retained
uncontrolled controller plus silent external updates avoids that caller-side copy.

Presentation includes three sizes, four variants, error/warning status, loading,
clear, prefix/suffix and icon slots. `option_render`, `label_render`, `tag_render`,
`overflow_render`, `popup_render`, and `empty_render` accept native elements.
Custom tags use `SelectTagRender::close` to preserve event and disabled behavior.
`SelectStyles` refines semantic parts, and `styles_fn` resolves them from current
size, variant, mode, status and interaction state. Component tokens use
`SelectTheme` / `SelectTokenPatch` through the established `ConfigScope` API.

The popup matches the trigger width by default. `popup_min_width` follows the
original's numeric width, clamping requests smaller than the trigger. Choose `PopupWidth` and
`PopupPlacement` explicitly when needed. Content-sized popups disable uniform
virtualization. `list_item_height` is the fixed-row contract for uniform lists;
choose `SelectListLayout::Variable` for measured custom rows and call
`invalidate_row_heights` if external row content changes without replacing options.
`max_tag_count` limits visible tags; `responsive_tags(true)` measures only tags
that can fit on one line. Both preserve all selected values.

Keyboard focus stays in the native editor for IME. A bounded logical list under
the combobox supplies active-descendant accessibility across the deferred visual
popup. Group headers do not contribute to option ordinals. Native Windows UIA
supports expansion, text search, option selection, clear, and tag removal.

## Slider

```rust,ignore
let slider = cx.new(|cx| {
    SliderState::with_options(
        SliderOptions { min: 0., max: 1., step: Some(0.01), ..Default::default() },
        window,
        cx,
    ).unwrap()
});

// In render:
Slider::new("volume", &slider)
    .scope(scope.clone())
    .accessibility_label("Volume")
    .on_change(|change, _, _| println!("{:?}", change.value))
```

`SliderValue` is `Single(f64)` or `Range(Vec<f64>)`. Set `SliderOptions::range`
to `Some(SliderRange::default())` for two or more handles. Empty and single-handle
range values are also supported. `set_options` and `set_value` validate finite
bounds, steps, values, and range settings before mutation. They return `Result`
and emit no user event. Builder `value` enables controlled proposals;
`default_value` initializes only once.

`step: None` snaps only to marks and endpoints. With a step, marks remain valid
snap targets alongside the grid. `dots` draws step ticks, `included` chooses a
continuous selected interval or independent points, and `orientation` / `reverse`
combine with the scope's direction. Numeric search and snapping do not enumerate
the step grid, and dense dot painting is bounded by the physical rail resolution.

`SliderRange::draggable_track` moves an interval while preserving its span.
`editable` adds handles on rail clicks and deletes a dragged handle after it moves
130 logical pixels away from the rail; moving it back before release restores it.
`min_count` / `max_count` constrain editing. Editable and draggable-track modes
are mutually exclusive. `SliderDisabled` can disable the whole control or an
array of individual handles; disabled handles form fixed crossing boundaries.

Handle identity and focus survive crossing. Arrow keys step, Page Up/Down use
two steps, and Home/End reach bounds. `SliderEvent::Change` reports each accepted
edit; `ChangeComplete` reports pointer/key release. Cancellation and unmount do
not send a late completion. `focus(index)` and `blur` use native focus handles.

`SliderMark` supports labels, custom native content and individual styles.
`SliderStyles` exposes root, rail, aggregate tracks, individual track segments,
handles, dots and marks. `styles_fn` resolves styles from current values,
options and interaction state. Tokens use `SliderTheme` / `SliderTokenPatch`.

`SliderTooltip` controls enabled/forced-open state, placement, overflow adjustment
and a native formatter. A formatter returning `None` suppresses that tooltip.
Closed, disabled and drag-deleted handles do not construct tooltip content.
`handle_labels` and `value_text` provide accessible names and formatted values;
Windows UIA exposes RangeValue and per-handle availability.

## Examples and verification

The gallery has all 27 public Select and 14 public Slider demo entries, including
two-way Slider/InputNumber synchronization. Its remote-user example uses a
deterministic local directory with asynchronous debounce and cancellation.

```powershell
cargo run --locked --offline --example gallery -- --component select
cargo run --locked --offline --example gallery -- --component slider
cargo test --locked --offline --lib select::
cargo test --locked --offline --lib slider::
cargo test --locked --offline --example gallery select_slider
./scripts/test-select-slider-accessibility.ps1
./scripts/run-select-slider-perf.ps1
```

The performance script requires the parent's configured
`windows-msvc-performance` Release preset. It records latency distributions and
allocation counts, with assertions on visible work and idle frame scheduling.
The browser fixture imports the actual pinned Ant Design source; capture scripts
record dependency integrity, source hashes, browser version and DPI. Current
verification results and remaining platform/rendering limits are recorded in
[the verification report](select-slider-progress.md).

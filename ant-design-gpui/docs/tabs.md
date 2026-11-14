# Tabs

Native GPUI implementation of Ant Design 6.6.7 Tabs, reference commit
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. There is no browser or JavaScript runtime
in the component. The reference checkout is only needed to regenerate developer fixtures.

## Ownership and integration

Create one controller per mounted tab group. Retain the item snapshot rather than
rebuilding pane factories on every render:

```rust,ignore
use ant_design_gpui::{TabItem, Tabs, TabsItems, TabsState};

let items = TabsItems::new([
    TabItem::new("overview", "Overview").text("Overview content"),
    TabItem::new("details", "Details").view(|window, cx| {
        cx.new(|cx| DetailsView::new(window, cx)).into()
    }),
])?;
let state = cx.new(|cx| TabsState::new(items, window, cx));

// In the owner view's Render implementation:
Tabs::new("project-tabs", &state).scope(scope.clone()).w_full()
```

`TabsItems` is an immutable, cheaply cloned snapshot with indexed key lookup.
Duplicate keys return an error; duplicate/localized labels are supported. Keep keys
stable while reordering. Replacing a factory deliberately replaces that pane's entity.

The controller owns selection, roving focus, measured geometry, header offset,
popup navigation and pane entities. `Tabs` is an inexpensive render builder.
Native measurement adapters preserve element identity and collect actual GPUI
layout bounds; no text-width estimates drive scrolling or indicator placement.
The overflow list uses `uniform_list`, rendering only its visible rows. The menu
measures all hidden labels (including custom content), icons and remove controls
to determine its width, with Ant Design's 120px minimum and 200px total height cap.
Rows fill the menu so hover, keyboard highlighting and pointer activation share
the same full-width target. Explicit popup widths also propagate to every row.

`active_key(...)` or `selected(Option<SharedString>)` enables controlled selection.
An activation emits a proposed `TabsChange`; the owner must accept it by updating
the value supplied on the next render. `default_active_key(...)` only initializes
uncontrolled selection. `set_active_key` and `set_items` are silent application
updates. Callbacks are deferred until the controller's mutable borrow is released,
so an owner can accept a change or replace items from a callback.

Editable tabs emit `TabsEdit::Add` and `Remove(key)` without changing application
items. `closable(false)`, disabled items, group disabling and `hide_add(true)` gate
the corresponding requests. The gallery demonstrates owner-defined selection
recovery after removal. `TabsEvent` also exposes activation (including repeated
activation), source information and bounded scroll changes.

## Supported behavior

| Area | Native API / behavior |
| --- | --- |
| Variants | `TabsType::{Line, Card, EditableCard}`; small, medium, large |
| Placement | `TabPlacement::{Top, Bottom, Start, End}`; logical sides resolve through scoped direction |
| Header | `centered`, `gutter`, `extra_start`, `extra_end`, custom add/remove/more icons |
| Indicator | Fixed or calculated length; start/center/end alignment; independent animation |
| Panes | Lazy factory creation, retained `AnyView`, `force_render`, group/per-item `destroy_on_hidden` |
| Overflow | Wheel/trackpad scrolling, automatic selection/focus reveal, native popup, virtualized rows, disabled rows, editable removal |
| Custom rendering | `header_render`, `tab_render`, `popup_render`; stable controller/context actions |
| Theme | Typed `TabsTheme`/`TabsTokenPatch`, global component overrides, inherited scopes and all four algorithms |
| Semantic styles | Root, header, item, remove, indicator, body, content and popup; optional style resolver |
| Accessibility | TabList, Tab, TabPanel and ListBox roles; selected/disabled states, labels, native focus and activation actions |

Arrow keys move focus without selecting. Horizontal arrows follow RTL; vertical
groups use Up/Down. Home/End jump to the first/last enabled tab. Enter/Space commits;
Delete/Backspace requests removal in editable groups. Tab traverses the tab stop,
close/add/more controls and the active pane. The overflow menu uses Up/Down,
Home/End and Enter/Space; Escape and outside dismissal are owned by the shared
Popup host. Custom popup content can supply `popup_focus(&handle)`.

`force_render` creates and retains a pane even while hidden. Without it, inactive
panes remain entities after their first visit unless destruction is enabled. State
that must survive belongs in the pane entity, rather than ephemeral element state.
Externally held entity handles remain owned by the application after destruction.
Incoming and outgoing opacity transitions and the indicator are interruptible;
reduced motion snaps to the final state. Idle or unmounted groups schedule no frames.
Exiting panes cannot accept pointer/keyboard activation, and focus leaving a removed
pane is recovered to a live tab.

## Gallery

`cargo run --locked --offline --example gallery -- --component tabs`

The first 16 cards correspond to the public reference examples. Four additional
cards cover independent animation, nested groups, a card container and component
tokens. The size card renders line, card and editable-card groups. Search filters
hidden tabs, skips disabled results and supports keyboard selection. Dragging uses
GPUI's native drag/drop system while preserving keyed pane identity. The sticky
example uses a bounded native ScrollArea and the header hook.

The gallery uses native Segmented controls for several reference Radio/Select
selectors. Its third icon is the pinned native Heart icon. Drag initiation follows
GPUI's native threshold (2 logical pixels) rather than dnd-kit's 10-pixel threshold;
the sticky example demonstrates a local viewport rather than a browser document.
These are deliberate native interaction adaptations.

## Focused verification

```powershell
cargo test --locked --offline --lib tabs::
cargo test --locked --offline --example gallery tabs_gallery
cargo clippy --locked --offline --lib --tests --example gallery --example tabs_visual --bench tabs -- -D warnings
./scripts/run-tabs-perf.ps1
```

The benchmark requires the configured `windows-msvc-performance` tree with Release
and benchmarks enabled. It measures 3, 30, 100 and 1,000 tabs, snapshot cloning,
idle drawing and selection/layout; it asserts zero idle animation callbacks.
The intended interactive workload is up to 100 tabs. The 1,000-tab case deliberately
stresses the fully measured header; only the overflow menu is virtualized. CPU
offscreen timings do not measure GPU presentation or end-to-end input latency.

The independent reference fixture contains 1,280 states (four themes, both
directions, sizes, variants, placements, icons, centered/custom/gutter cases,
rest/hover/pressed/focus). Tests compare 3,840 individual tab styles. Generation
executes the original upstream style functions and cssinjs against the pinned theme
fixture, with snapshot-derived DOM. This verifies CSS appearance, not React event
behavior. No older cached rc-tabs release is treated as an exact behavior oracle.

```powershell
node scripts/generate_tabs_reference.mjs --upstream E:/ant-design --pnpm-store E:/.pnpm-store/v10 --browser 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'
cargo run --locked --offline --example tabs_visual -- --output ../build/ant-design-gpui/tabs-native
node scripts/capture_tabs_reference.mjs --output ../build/ant-design-gpui/tabs-browser --scale 1.5
python scripts/compare_tabs_visual.py --native ../build/ant-design-gpui/tabs-native --reference ../build/ant-design-gpui/tabs-browser --output ../build/ant-design-gpui/tabs-visual-report.json
cargo run --locked --offline --example gallery -- --component tabs --headless --reduced-motion --capture ../build/ant-design-gpui/tabs-gallery.png
```

The native capture uses real GPU rendering in a hidden window and exits. Browser
capture owns an isolated headless Edge process. The comparison does no registration,
resampling or font masking; it reports component and foreground error plus native
item geometry. Generated PNGs, HTML, browser metadata and reports remain in ignored
build directories. Native/browser DPI must match.

### Recorded results

Validated on 2026-10-28 with Windows x64, Ryzen 9 5950X, Rust 1.98.1,
and the repository's pinned GPUI revision. The 24 component tests and five gallery
integration tests passed. Targeted Clippy passed with warnings denied.
The committed [validation report](../fixtures/tabs/validation-windows-150.json)
contains all 344 native/browser image comparisons and the Release timings below.

| Tabs | Warm draw median / p95 | Selection and layout median / p95 |
| ---: | ---: | ---: |
| 3 | 0.100 / 0.135 ms | 0.195 / 0.210 ms |
| 30 | 0.463 / 0.496 ms | 1.404 / 1.485 ms |
| 100 | 1.417 / 1.517 ms | 4.354 / 4.922 ms |
| 1,000 | 24.270 / 33.766 ms | 65.702 / 93.031 ms |

Each draw/selection case contains 200 samples. Snapshot cloning was at most
0.10 microseconds at p95 across 10,000 samples per size. Idle animation callbacks
were zero. The 100-tab interactive target is met; the 1,000-tab stress case exceeds
a 60 Hz frame budget and is not an interactive performance guarantee.

The images cover all theme/direction/size/variant/placement combinations, plus
hover, pressed and keyboard focus for medium top line/card tabs, and centered
line tabs in both directions. Mean absolute channel error over the component
region was 2.450/255 across the 344 pairs; the worst pair was 5.004/255. This aggregate includes background pixels and must not
be read as a pixel-perfect score. The report also includes foreground-only error.
Maximum per-tab geometry differences were 2.833 logical pixels in width,
0.667 in height, and 1.333 in vertical position; horizontal position differences
accumulated to 8.5 pixels across three tabs.

Windows at 150% DPI is the verified native target. Small native text-advance and
fractional-pixel border/padding differences remain; the implementation uses the
reference font stack and token values rather than compensating with font-specific
width constants. macOS/Linux rendering, other DPI settings, touch interaction and
screen-reader announcements require native validation. AccessKit tab/panel roles
and labels are present, but explicit DOM-style IDREF relationships are not exposed
by this GPUI version's div API.

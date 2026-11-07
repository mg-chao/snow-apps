# Menu

`menu::Menu` is persistent application navigation with horizontal, vertical and
inline layouts. It follows the Ant Design 6.6.7 reference checkout. The existing
`ContextMenu` remains available for actions opened at a pointer or keyboard target.

Create the model and retained controller once, then render fresh Menu builders
with stable IDs:

```rust
use ant_design_gpui::menu::{Menu, MenuItem, MenuItems, MenuMode, MenuState, SubMenu};
use gpui::{AppContext, prelude::*, px};

// In the owning view's constructor:
let items = MenuItems::new([
    MenuItem::new("home", "Home").into(),
    SubMenu::new("workspace", "Workspace", MenuItems::new([
        MenuItem::new("documents", "Documents").into(),
        MenuItem::new("settings", "Settings").into(),
    ]).unwrap()).into(),
]).unwrap();
let state = cx.new(|cx| MenuState::new(items, window, cx));

// In render:
Menu::new("navigation", &state)
    .scope(scope.clone())
    .mode(MenuMode::Inline)
    .default_selected_keys(["home"])
    .default_open_keys(["workspace"])
    .w(px(256.));
```

Import navigation types from `ant_design_gpui::menu`; the crate root's existing
`MenuItem`, `MenuItems` and `MenuEvent` names belong to `context_menu` for backwards
compatibility. Menu keys are stable strings independent of labels and positions.
`MenuItems::new` rejects duplicate keys anywhere in the tree, including groups and
dividers. Collections are immutable shared snapshots; `set_items` replaces a
controller's model without rebuilding it during every render.

`MenuItem` supports icons, extra content, disabled and danger states, collapsed
tooltip titles and custom render factories with an accessible text fallback.
`SubMenu`, `MenuGroup` and `MenuDivider` represent branches, titled groups and
separators explicitly. The host application supplies localized labels.

## Selection and open branches

Default key builders initialize retained state when the controller is first
rendered. `selected_keys` and `open_keys` make those values controlled. User actions
report proposed values through `on_event`, and the owner supplies the next keys. Selection
and opening can be controlled independently. Use `multiple(true)` for independent
selection or `selectable(false)` for navigation actions without selection.
Multiple selection exposes native menu-item checkbox semantics and checked states;
single navigation destinations use current-page metadata and native Invoke.

`MenuEvent` distinguishes activation, selection, deselection, submenu title
activation, open-key changes and focus changes. Activation includes the stable
key, innermost-first key path, label and input source. Programmatic
`set_selected_keys`, `set_open_keys` and `set_items` are silent; they do not imitate
user input or produce duplicate callback events.

Reactivating the current single-selection item emits `Activate` without a
selection-change event. `Deselect` reports removal from a multiple selection.
This follows the documented callback contract. The pinned `@rc-component/menu` 1.5.0
implementation instead calls `onDeselect` when an already-selected single item
is activated; this callback quirk is a deliberate behavioral difference.

## Layout and customization

- `MenuMode::Horizontal` lays out top navigation with an overflow submenu when
  items no longer fit. Vertical menus open adjacent popup branches. Inline menus
  expand their branches in place.
- `inline_collapsed(true)` turns inline navigation into an icon rail. Leaf titles
  appear as tooltips; `tooltip(false)` disables them and `tooltip_placement` changes
  their placement. Expanded inline open keys are restored when the rail expands.
- `MenuColorTheme::Light` and `Dark` choose Menu palettes independently of the
  provider's token algorithm. A `SubMenu` may override its popup palette.
- `MenuStyles` supplies semantic GPUI refinements. `MenuTheme` and
  `MenuTokenPatch` integrate with the existing scoped component theme system.
- `max_height` bounds a persistent root viewport. Large bounded roots and popup
  branches use native lazy rows, retaining keyboard navigation to distant items.
- `MenuItem::content` receives a `MenuRenderContext` and creates native content.
  `popup_render` receives the normal submenu content and `MenuPopupContext`; return
  a native element wrapping that content to preserve navigation behavior.

Long virtualized popups measure custom content from a bounded sample and the
active item. Their width can grow when a wider item enters that sample. Use
`MenuStyles { submenu_list: StyleRefinement::default().w(px(280.)),
..Default::default() }` when the application needs a stable width. Plain text
labels use a cached measurement of the complete list.

Arrow keys move between enabled items and submenu levels; Home and End reach the
first and last items. Enter and Space activate, Escape closes the current popup
level, and Tab leaves the menu. Letter keys provide typeahead navigation. Direction
and motion follow the retained configuration scope. Disabled items and titled
groups do not activate.

## Gallery and focused verification

The gallery includes all 11 public Menu reference examples in their source order:

```powershell
cargo run --locked --offline --example gallery -- --component menu
cargo run --locked --offline --example gallery -- --component menu --dark --rtl --compact
cargo test --locked --offline --example gallery -- menu_gallery --test-threads=1
cargo test --locked --offline --lib menu:: -- --test-threads=1
./scripts/test-menu-accessibility.ps1
./scripts/run-menu-perf.ps1
```

The gallery's theme, density, direction and motion controls preserve retained Menu
state. The accordion example demonstrates owner-controlled open keys; the custom
popup example uses the same selectable native rows inside a richer surface.

The final focused run passed 149 library tests: 46 Menu, 75 shared-popup,
21 Context Menu and seven locale tests. All four Menu gallery tests and the
catalogue regression passed. Strict Clippy passed for the library, both Menu
capture/accessibility examples, the Menu benchmark and the gallery.

The native accessibility script exercises its own Windows fixture and records
evidence under `build/ant-design-gpui/menu-uia/`. The focused performance script
requires the configured `windows-msvc-performance` Release tree and writes
CPU/layout samples under that preset's `ant-design-gpui/menu-results.txt`. It checks
bounded row construction and idle frame scheduling; these measurements do not
represent GPU presentation time or input latency.

The 2026-10-28 Windows UIA run passed native menu, menu-bar and menu-item roles,
disabled-item skipping, Expand/Collapse actions and metadata, keyboard selection,
native Invoke, activation source, popup dismissal and horizontal navigation.
It also passed menu-item checkbox roles, checked metadata and independent native
Toggle actions for multiple selection. Six leaf activations, including the two
native selection toggles, each fired exactly once.

The independent Ant Design style fixture covers 55 modern component tokens,
640 computed menus and 3,968 rows. Executable comparison inputs contain 88 main
scenarios and nine structural edge cases at 100%, 150% and 200% DPI. See the
[fixture guide](../fixtures/menu/README.md) for capture and comparison commands.
All 291 final browser/native pairs passed 2,928 checks: 690 surface/focus,
1,641 foreground-color, 291 root-layout, 282 default-chevron bounds and
24 custom-expand glyph bounds checks.
The [verification ledger](../fixtures/menu/verification.json) maps features to
specific tests and records the final run hashes, counters and report paths.

The pinned GPUI backend propagates notified views to their ancestors
([`mark_view_dirty`, line 2264](https://github.com/mg-chao/gpui/blob/2c0bf70b54c018f99419f386d1cf1105dca57661/crates/gpui/src/window.rs#L2264)).
Moving native focus also refreshes the whole window
([`Window::focus`, line 2418](https://github.com/mg-chao/gpui/blob/2c0bf70b54c018f99419f386d1cf1105dca57661/crates/gpui/src/window.rs#L2418)).
The benchmark reports that ancestor and focus-refresh work explicitly. Its cache
check applies to unrelated cached siblings during state changes; native focus
movement can redraw those siblings as part of the backend's window refresh.

Release measurements on 2026-10-28 used an AMD Ryzen 9 5950X, Rust 1.98.1, GPUI's
offscreen text system and motion disabled. Representative p50 CPU/layout samples:

| Work | p50 |
| --- | ---: |
| Build 1,000-item model | 2.928 ms |
| Clone shared 1,000-item model | 2.1 ns |
| Look up a key in 1,000 items | 25.8 ns |
| Cached bounded 1,000-item root redraw | 32.7 µs |
| Cached closed 1,000-item submenu redraw | 19.4 µs |
| Cached open 1,000-item submenu redraw | 162.5 µs |
| Navigate a 1,000-item submenu, including native focus refresh | 0.944 ms |

The 1,000-item root content factories ran 14 times initially and 41 during
distant End-key navigation. Popup content factories ran 42 times initially and
at most 56 times per measured redraw. The popup totals include 16 custom-width
measurement calls as well as visible-list construction. Across 105 programmatic
selection updates, unrelated cached sibling content rendered zero additional
times; the ancestor rendered 105 times.
Settled menus requested zero animation frames. Clone and lookup samples batch
1,000 operations to avoid the Windows timer's sub-operation quantization. These
figures are local observations rather than cross-machine performance guarantees.
`menu-provenance.json` beside the results records the measured executable and
source SHA-256 hashes; the recorded Menu/shared-popup inputs stayed unchanged
during the run. The benchmark retained its existing bound of fewer than 128
content factory calls per measured initialization or redraw.

Popup entrance and exit use native fade/reveal motion. The reference scales its
popup subtree; the pinned GPUI revision cannot apply that transform to arbitrary
native content. Settled layouts are compared separately from this agreed motion
difference. Text antialiasing and shadow rasterization also depend on the native
renderer; screenshot reports retain their unmasked pixel errors.

Run only focused checks for changes to Menu and shared infrastructure. Native
backend validation on macOS and Linux requires those platforms; Windows evidence
does not establish cross-platform screenshot or accessibility parity. Interactive
screen-reader sessions were not performed; UI Automation checks establish the
native roles, properties and actions exercised by the fixture.

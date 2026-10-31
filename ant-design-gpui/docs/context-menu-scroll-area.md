# Context Menu and Scroll Area

Both components use retained GPUI entities and the existing scoped theme system.
The Qt siblings supplied the interaction reference; layout, event dispatch and
lifetime ownership are implemented with native GPUI elements. There are no new
dependencies or rasterized widget surfaces.

## Context Menu

Create an immutable item snapshot and retain one controller per trigger:

```rust,ignore
use ant_design_gpui::{ContextMenu, ContextMenuState, MenuEntry, MenuItem, MenuItems};

let items = MenuItems::new([
    MenuItem::new("copy", "Copy").shortcut("Ctrl+C").into(),
    MenuItem::new("hidden", "Show hidden files").checked(false).into(),
    MenuEntry::Separator,
    MenuItem::new("delete", "Delete").danger(true).into(),
])?;
let menu = cx.new(|cx| ContextMenuState::new(items, window, cx));

// Rebuild this element during render; retain `menu` in the owning view.
ContextMenu::new("file-context", &menu)
    .scope(scope.clone())
    .accessibility_label("File actions")
    .child(file_preview)
    .on_event(|event, window, cx| {
        // Handle MenuEvent::Activate or subscribe to the retained entity.
    })
```

Item identifiers are stable GPUI `ElementId`s, independent of translated labels.
They must be unique within each menu level; `MenuItems::new` returns an error for
duplicates or nesting deeper than the supported limit. Cloning an item snapshot
shares its immutable storage. `set_items` replaces the authoritative snapshot.

`MenuItem` supports `icon`, `shortcut`, `badge`, `disabled`, `danger`, `checked`,
`radio(group, checked)`, `submenu(items)` and `lazy_submenu(factory)`.
Use `MenuEntry::group(label)` for a noninteractive group heading. Shortcuts are
display hints: command bindings belong to the host application. A lazy factory
runs when its branch opens; closing the branch releases that transient content.
Check and radio state is retained by the controller and included in activation
events. The owner can replace it with a new item snapshot.

Right-click opens at the pointer. Focus the trigger and use Shift+F10 or the Menu
key to open from the keyboard. Arrow keys move between enabled items and nested
levels, Home/End reach the first/last item, Enter/Space activate, and printable
characters perform typeahead. Left/right branch navigation follows the effective
direction. Escape closes a branch before dismissing the root; Tab dismisses and
continues normal focus traversal. Activation and dismissal remove the popup's
input and accessibility subtree immediately.
Plain targets expose a named group with menu metadata and the accessibility
`ShowContextMenu` action. Existing Button targets retain their own accessible
identity and menu expansion action.

Menus with more than 64 entries use GPUI's variable-height list; separators and
group headings retain their natural heights. Per-level text measurements are
cached against immutable item, theme and font snapshots. Keyboard navigation
reveals the active row without building every offscreen command.
The initial width measurement shapes the labels once; performance measurements
of reopening and redraw use warmed caches.

The existing `UiRoot` popup host supplies clipping escape, collision adjustment,
nested popup ownership and outside dismissal. Menu surfaces inherit typography,
colors, density, direction and motion from `ConfigScope`.
The default accessible menu and scrollbar names are localized in English,
Simplified Chinese and Traditional Chinese; custom locale fields inherit through
the usual `LocalePatch` mechanism. Application item labels remain caller-owned.

## Scroll Area

```rust,ignore
use ant_design_gpui::{ScrollArea, ScrollAreaState, ScrollAxis};
use gpui::{prelude::*, px};

let scroll = cx.new(|cx| ScrollAreaState::new(window, cx));

ScrollArea::new("documents", &scroll)
    .scope(scope.clone())
    .axis(ScrollAxis::Vertical)
    .accessibility_label("Documents")
    .w_full()
    .h(px(320.))
    .child(document_list)
```

Give the viewport a finite height or a bounded flex layout. Vertical mode fits
content to the viewport width and measures natural content height. Horizontal
and both-axis modes permit wider content. Overlay scrollbars occupy no layout
space. Their proportional thumbs have a minimum hit size, grow visually on
hover, and reserve the shared corner when both axes overflow.

`ScrollbarVisibility::Always`, `Hover` and `Hidden` control presentation. Hidden
bars retain wheel, trackpad, keyboard and application scrolling. Native wheel
deltas use the configured axes; Shift+wheel selects the horizontal axis when it
is available. A region consumes a wheel event only when its offset moves, so
scrolling at an edge can reach an enclosing region.

Focus a region to use Arrow/Page/Home/End keys. Drag a thumb or click its track to
scroll. The vertical scrollbar follows the logical trailing edge in RTL. Respect
the configured content direction independently when building rows or text.

`ScrollAreaState` exposes `offset`, `max_offset`, `scroll_to`, `scroll_by`,
`scroll_to_top`, `scroll_to_bottom` and the native `handle`. GPUI uses negative
offsets as content moves upward or leftward. Anchors and application controls
operate on the same handle as wheel and scrollbar input. Use `on_scroll` or
subscribe to `ScrollAreaEvent` for offset changes.
Use `ScrollAnchor::for_handle` for individual descendants; the native handle's
`scroll_to_item` indexes the area's internal content wrapper rather than those
descendants. Track presses page once; holding the track does not auto-repeat.

The component clips arbitrary GPUI content; it does not virtualize arbitrary
children. Use GPUI's virtualized list APIs directly for very large collections.
Those lists own their scrolling; nesting one here does not connect its internal
scroll handle to these outer scrollbars.

## Theme overrides

Both controls expose typed component tokens. Sparse patches inherit through the
same scope mechanism as the existing controls:

```rust,ignore
use ant_design_gpui::scroll_area::{ScrollAreaTheme, ScrollAreaTokenPatch};

scope.update_patch(cx, |patch| {
    patch.theme.get_or_insert_default().components.set::<ScrollAreaTheme>(
        ScrollAreaTokenPatch {
            thickness: Some(10.),
            collapsed_thickness: Some(4.),
            ..Default::default()
        },
    );
})?;
```

`ContextMenuTheme` and `ContextMenuTokenPatch` configure row height, spacing,
icons, surface/item radii, minimum width and semantic colors. `ScrollAreaTheme`
configures thumb thickness, inset, minimum length and idle/hover/active colors.
Visual transitions honor scoped and platform reduced-motion settings; settled
components schedule no continuing animation frames.

## Verification

Run only the relevant selections:

```powershell
cargo test --locked --offline --lib context_menu::
cargo test --locked --offline --lib scroll_area::
cargo test --locked --offline --lib popup::
cargo test --locked --offline --lib locale::
cargo test --locked --offline --example gallery context_scroll
cargo test --locked --offline --example gallery catalogue_keeps_reference_order
cargo clippy --locked --offline --lib --tests --example gallery --example context_scroll_visual --bench context_scroll -- -D warnings
./scripts/run-context-scroll-perf.ps1
cargo run --locked --offline --example context_scroll_visual -- --output ../build/ant-design-gpui/context-scroll-native
```

The performance script requires the repository's `windows-msvc-performance`
build tree, Release configuration and enabled benchmarks. It measures only these
components. Native visual review can use `--component context-menu` or
`--component scroll-area`, combined with the gallery's light/dark, compact,
RTL and reduced-motion switches. macOS and Linux require native verification
on those platforms.

The GPUI test backend does not initialize a live accessibility tree. Tests check
semantic node metadata directly and verify painted navigation and native list
geometry. Live Windows UI Automation and screen-reader sessions are not covered
by these offscreen tests or the native image captures.

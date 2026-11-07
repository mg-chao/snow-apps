# Notification

Notification is a native GPUI overlay for persistent or timed feedback. It follows
Ant Design 6.6.7 while using retained entities, scoped configuration, native focus,
and a single controller timer. It has no browser or JavaScript runtime dependency.

## Open from the current window

`UiRoot` owns and mounts a default controller. Obtain its weak service inside a
window callback:

```rust
use ant_design_gpui::{Notification, NotificationOptions};

let api = Notification::for_window(window, cx).expect("window has a UiRoot");
let handle = api.success(
    NotificationOptions::new("Export finished")
        .description("All selected images were exported.")
        .key("export"),
    window,
    cx,
);
```

`info`, `success`, `warning`, and `error` accept the same options as `open`.
Defaults are top right, 4.5 seconds, a close button, hover pausing, and a stack
threshold of three. A zero duration keeps the notice open and omits its progress
bar. The six `NotificationPlacement` values refer to physical window edges;
RTL changes content alignment and close-button position without swapping placement.
The action footer stays on the physical right, matching the reference; native
action factories can read their scope direction when arranging multiple controls.

`title` and `description` accept text. `title_content`, `description_content`,
`icon_content`, and `actions` accept factories returning native elements. Retain
application state in entities and clone them into factories. Bodies are cached
across countdown redraws; factories run again when their options, width, DPI,
configuration, or semantic styles change. They may synchronously dismiss their
own handle. Rich content should provide an explicit accessible label/description
when the text cannot be inferred.

`icon` and `close_icon` accept the existing `IconRef` system, including custom SVG
and explicit colors. `show_icon(false)`, `closable(false)`, `close_disabled(true)`,
and `close_label` control icon and dismissal presentation. Close buttons support
pointer, Enter, Space, native UI Automation invocation, and visible keyboard focus.
Action content receives its own lifetime's handle, so its callback can close the
correct notice without looking up a mutable key.

## Lifetime and ownership

Opening an active key updates that notice without reordering it. An unchanged
duration preserves the remaining budget; changing duration starts a fresh budget.
Changing placement moves the same lifetime. `destroy(key)` and `destroy_all()`
settle matching lifetimes. An old handle cannot dismiss a later reuse of its key.

`handle.close(window, cx)` dismisses programmatically. `handle.completion()` is an
awaitable future returning a `NotificationCloseEvent`; it resolves at logical
dismissal, before the exit animation finishes. Reasons distinguish timeout, close
button, programmatic close, destruction, count eviction, owner release, and an
unavailable controller. Dropping a completion future removes its waker without
closing the notification. Completion and `on_close` run once; callbacks execute
outside controller borrows and may open or close other notices.

Services and handles hold weak controller references. Retain a `NotificationState`
entity and mount `NotificationHost::new(id, &state, content)` for an independent
scope. Create that state with `NotificationState::new(scope, window, cx)` in
`cx.new`. A controller belongs to one native window and one active host. Unmount
or window teardown cancels work, releases content, and settles outstanding futures
with `OwnerDropped`; no UI callbacks are invoked after ownership ends. Calls using
the wrong window are rejected. A newly mounted host can reuse a retained controller.

## Configuration and appearance

Set `ConfigPatch::notification` on a retained `ConfigScope`, or call
`api.set_config(NotificationConfigPatch { ... }, window, cx)` for controller-local
overrides. `set_config` replaces that local patch; omitted fields inherit the
scope. Per-notice options take precedence. Configuration changes update active
appearance and limits without resetting existing duration budgets. Top/bottom
insets default to the current theme's margin token. `max_count: Some(Some(n))`
keeps the newest n active notices across all placements; zero or `Some(None)`
removes an inherited limit.

`NotificationStack::Disabled` displays the full list. `Enabled { threshold }`
collapses it only when its count exceeds the threshold (at least one). Up to
three card surfaces remain visible. Hover or descendant keyboard focus expands
the group. Keyboard focus always pauses expiry; `pause_on_hover(false)` opts out
of pointer pausing. Expanded lists scroll within the native window, including
bottom placements. New notices do not steal focus. Dismissal restores prior focus
when it was inside the notice and excludes outgoing controls from interaction.

`NotificationTheme` exposes width, popup z-index, progress background, and the
four status backgrounds through `NotificationTokenPatch`. Backgrounds support
native solid colors and gradients. Font, spacing, radius, shadow, color, and
motion derive from the effective theme; default/dark/compact/dark compact and RTL
are covered by the visual fixtures. Scoped and application reduced motion remove
entry, exit, stack, and reflow motion; timed progress still represents elapsed time.

`NotificationStyles` has typed GPUI refinements for `list`, `list_content`, `root`,
`wrapper`, `section`, `icon`, `title`, `description`, `actions`, `close`, and
`progress`. Holder styles belong to configuration; individual options refine card
slots. `styles_fn` receives status, placement, direction, motion, closing, stacking,
and close/progress visibility. Native list/content padding and gap participate in
layout. For an absent semantic slot (for example, a wrapper without an icon), its
style has no effect. Styles use GPUI's native sizing and layout rules.

Countdowns use monotonic deadlines and one earliest-deadline task per controller.
Animations share one guarded next-frame callback. Empty, settled, persistent,
hidden, and entirely offscreen animated content do not sustain redraw loops.

## Verification and gallery

The gallery includes all twelve applicable public reference examples plus the
component-token example:

```powershell
cargo run --locked --offline --example gallery -- --component notification
cargo test --locked --offline --lib notification:: -- --test-threads=1
cargo test --locked --offline --example gallery notification_gallery
./scripts/test-notification-accessibility.ps1
./scripts/run-notification-perf.ps1
```

See [verification](notification-verification.md) for the independent reference,
reproduction commands, recorded measurements, and renderer/platform limitations.

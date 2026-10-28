# Message

Message displays temporary, nonmodal feedback at the top of a native window.
The implementation follows Ant Design 6.6.7 while using retained GPUI entities,
native layout, scoped themes, and the existing icon renderer.

## Window API

`UiRoot` owns a default controller and mounts its host. Inside a callback,
retrieve that window's API:

```rust
use ant_design_gpui::Message;

// Inside a native event callback:
if let Some(messages) = Message::for_window(window, cx) {
    messages.success("Document saved", window, cx);
}
```

`UiRoot::messages()` returns the same API. `info`, `success`, `error`, `warning`,
and `loading` accept text and return a `MessageHandle`. Each service belongs to
one window; passing a different window does not transfer its notices.

## Scoped ownership

For an independent message list, retain one `Entity<MessageState>` in the owning
view. Create it once with
`cx.new(|cx| MessageState::new(scope.clone(), window, cx))`. Render
`MessageHost::new("local-messages", &state, content)` around that view's content,
and retain `Message::new(&state)` in event or asynchronous handlers.

The host preserves its child's layout and paints notices in a deferred window
layer. Mount a controller once. An independent host uses its own configuration,
count limit, timers, and stack. It still positions notices relative to the
window, so separate hosts should be used deliberately to avoid overlapping lists.

Services and handles hold weak references. Dropping a handle does not dismiss
the notice or retain a window. Unmounting its owner or closing the window clears
the notices and settles pending completion futures.

## Content and lifetime

```rust
use std::time::Duration;
use ant_design_gpui::message::{Message, MessageOptions, MessageType};

let handle = messages.open(
    MessageOptions::new("Uploading document...")
        .kind(MessageType::Loading)
        .key("upload")
        .duration(Duration::ZERO),
    window,
    cx,
);

// Replace the same logical notice when the upload finishes.
messages.open(
    MessageOptions::new("Upload complete")
        .kind(MessageType::Success)
        .key("upload")
        .duration(Duration::from_secs(2)),
    window,
    cx,
);

// Or dismiss the logical notice explicitly.
handle.close(window, cx);
```

The default duration is three seconds; zero makes the notice persistent. A plain
`MessageOptions::new(...)` has no status icon. `.kind(...)` selects one of the
five built-in icons; `.icon(...)` supplies a custom icon and `.show_icon(false)`
suppresses it. `.on_click(...)` handles pointer activation without automatically
closing the notice. `destroy(key, ...)` removes one notice; `destroy_all(...)`
removes the list.

Keys are native `SharedString` values. Updating a live key preserves its lifetime
and list position. Matching the upstream timer behavior, an update with the same
effective duration preserves the existing deadline; changing the duration starts
the new duration. An outgoing key can be opened again as a new lifetime.
Native measurements also keep stack placeholders synchronized when keyed
content changes size, improving on the reference's mount-only size tracking.

`MessageOptions::content(|handle, window, cx| element)` supplies a native content
factory for rich text, custom layouts, or actions. The factory runs when the
host rebuilds its card, so keep application state outside the factory. Give rich
content an explicit accessible label when its meaning cannot be derived from
the child elements.

Plain text collapses normal whitespace and wraps at Unicode line-break
opportunities. Matching the measured reference, an unbroken token can extend
outside its card in a narrow window and is clipped at the viewport edge.

## Completion

`handle.completion().await` yields `MessageCloseEvent { key, reason }` when
dismissal begins. `.on_close(...)` runs at that logical dismissal. The outgoing
visual may continue animating afterward. Every lifetime settles once, and
cloned handles observe the same completion. Reasons distinguish timeout,
programmatic close, destruction, count eviction, owner release, and an unavailable
controller. Owner release settles futures without invoking callbacks against a
window that no longer exists.

This gives Rust tasks a native alternative to the reference's JavaScript
thenables. The gallery's sequential example awaits loading completion, then
opens success and information notices. Retain the task in the application view
so it can be cancelled when the surrounding operation is replaced.

## Configuration and appearance

Use `ConfigPatch::message` for inherited defaults. `Message::set_config` or
`MessageState::set_config` replaces the controller's local `MessageConfigPatch`.
The patch supports logical-pixel `top`, default `duration`, `max_count`,
`pause_on_hover`, stack mode, semantic styles, and a style resolver.
`max_count: Some(None)` removes an inherited limit. Zero also means no limit.

`MessageStack::Disabled` is the default. `MessageStack::Enabled { threshold: 3 }`
collapses the list when its count exceeds three: the latest notice remains
visible above two small placeholder surfaces. Hover expands the list. Individual
hover pauses a notice by default; collapsed stack behavior also keeps the
collection available while the pointer explores it. The list scrolls when its
expanded content exceeds the viewport.

Register `MessageTheme` through the existing component-theme system.
`MessageTokenPatch` exposes `content_bg`, `content_padding: Edges<f64>`, and
`z_index_popup`. Component-local global token overrides control radius,
typography, status colors, shadows, spacing, and motion. Theme, density, direction,
and motion updates retain active notice lifetimes.

`MessageStyles` exposes `list`, `list_content`, `root`, `wrapper`, `icon`, and
`title` slots as GPUI `StyleRefinement` values. Configure `list` and `list_content`
through the scope or controller's `MessageConfigPatch`; these slots belong to
the host. Per-notice `MessageOptions::styles` and `styles_fn` apply only to
`root`, `wrapper`, `icon`, and `title`, matching the reference's ownership.
Resolvers receive `MessageStyleContext` with the optional kind, direction,
motion, closing, and stacked state. Host card styles precede per-notice styles;
within each level, the resolver refines its static styles.

The default accessible role is Alert, with an assertive atomic live region.
`.accessibility_role(Role::Status)` selects polite announcements instead.
Passive notices do not take focus or add a tab stop; native content actions keep
their own semantics. Reduced motion immediately settles transitions and stops
loading rotation. Settled static lists and empty hosts request no animation
frames. One controller timer tracks the next expiry rather than spawning one
timer per render.

## Focused verification

```powershell
cargo run --locked --offline --example gallery -- --component message
cargo test --locked --offline --lib message:: -- --test-threads=1
cargo test --locked --offline --example gallery message_gallery
pwsh -NoProfile -File scripts/test-message-accessibility.ps1
pwsh -NoProfile -File scripts/run-message-perf.ps1
```

See [the verification guide](message-verification.md) and
[independent fixtures](../fixtures/message/README.md) for comparison details.

# Native popups

`Tooltip`, `Popover`, and `Popconfirm` share the public `popup::Popup` foundation.
Use stable, locally unique IDs and supply a fresh trigger element during render.
`UiRoot` installs the window's host automatically. A custom root can wrap its
content in `PopupHost::new("popups", content)`.

```rust
use ant_design_gpui::{Button, Tooltip};

Tooltip::new("save-tip")
    .title("Save the current document")
    .child(Button::new("save").label("Save"));
```

Pass `.scope(scope.clone())` to both the popup and its trigger to inherit a
window or subtree's theme, direction, locale, and defaults. Tooltip and Popover
default to hover plus keyboard focus. Popconfirm defaults to click and keyboard
activation. `PopupTriggers` supports `|` combinations and context-menu invocation.
Hover entry and exit delays default to 100 ms.

## Content and visibility

Tooltip accepts `.title(...)` or `.title_content(...)`. Empty title text suppresses
it. Popover accepts an optional title plus `.text(...)` or `.content(...)`.
Factories receive a weak `PopupHandle`, the current `Window`, and `App`:

```rust
use ant_design_gpui::{Button, Popover, popup::PopupTriggers};

Popover::new("details")
    .title("Details")
    .triggers(PopupTriggers::CLICK)
    .content(|handle, _, _| {
        let handle = handle.clone();
        Button::new("close")
            .label("Close")
            .on_click(move |_, window, cx| handle.close(window, cx))
    })
    .child(Button::new("details-trigger").label("Details"));
```

`.default_open(...)` initializes uncontrolled state once. `.open(value)` makes
the application authoritative; `.on_open_change(...)` reports a requested value
and `PopupOpenReason`. Accept a request by changing application state and calling
`cx.notify()`. Declining a request leaves the controlled value unchanged and does
not consume the next user activation. Open-change callbacks work with
`cx.listener(...)`.

Keep a `PopupHandle` in application state and connect it with `.handle(&handle)`
for programmatic requests, explicit realignment, or inspection of the most recent
layout. Handles do not keep unmounted controls alive. `PopupSession` identifies
both a mounted instance and its current opening, for guarding external async work.

Content factories run only while presented. A hidden popup retains its last
factory and captured entities; `.fresh(true)` updates that description while
hidden. `.destroy_on_hidden(true)` releases popup-owned content on closure,
including when `fresh` is also enabled. Keep meaningful editable state in owned
GPUI entities captured by the factory. Frame-local elements and controls' internal
keyed state are not retained across a hidden subtree.

## Confirmation

Popconfirm uses native small Buttons and locale-provided OK/Cancel labels.
`.ok_text(...)`, `.cancel_text(...)`, `.show_cancel(false)`, and
`PopconfirmButtonOptions` customize the actions. `.icon(...)` replaces the warning
icon; `.show_icon(false)` removes it.

The confirmation callback returns `Confirmation::Close`, `KeepOpen`, or
`Pending(Task<Result<(), SharedString>>)`. `.on_confirm_async(...)` is shorthand
for returning a pending task. These callbacks return a value, unlike
`cx.listener(...)`; use a closure that updates a weak application entity, as the
gallery demonstrates.

```rust
use ant_design_gpui::{Button, Popconfirm};
use std::time::Duration;

Popconfirm::new("delete")
    .title("Delete the item?")
    .description("This operation cannot be undone.")
    .on_confirm_async(|_, _, cx| {
        let timer = cx.background_executor().timer(Duration::from_secs(1));
        cx.spawn(async move |_| {
            timer.await; // Replace this with application work.
            Ok(())
        })
    })
    .on_confirm_error(|error, _, _| eprintln!("{error}"))
    .child(Button::new("delete-trigger").label("Delete").danger(true));
```

Pending confirmation shows loading and rejects duplicate confirmation. Success
requests closure; failure clears loading and calls the error callback while
keeping the dialog open. Cancel remains available. Dismissal or unmounting does
not cancel the application's task, and a stale completion cannot close a later
opening. Only the Cancel button invokes `on_cancel`; outside press and Escape
have their own open-change reasons.

Keyboard opening focuses Cancel, falling back to Confirm when Cancel is absent
or disabled. Escape closes the topmost popup. Confirm, Cancel, and Escape restore
trigger focus when focus was in the popup; outside dismissal preserves the outside
control's activation and focus.

## Native accessibility and custom triggers

Button, Checkbox, Radio, Switch and actionable Tag triggers keep their existing
accessible identity and tab stop. Tooltip text extends the control's description.
Popover and Popconfirm expose their expanded state and dialog actions. Dialog
titles and plain descriptions become accessible names and descriptions;
`.accessibility_label(...)` and `.accessibility_description(...)` supply these
for custom content. Pending confirmation exposes busy state and removes the
Confirm button's activation action while retaining its keyboard tab stop.

Windows exposes expanding controls through UI Automation's `ExpandCollapse`
pattern, rather than `Invoke`. Expand and Collapse request visibility with
`PopupOpenReason::Accessibility`; these explicit visibility actions do not call
the trigger's click callback. Native accessible Click (on platforms that expose
it), pointer activation, Enter and Space preserve the control's callback. Native
accessible opening also focuses the safe confirmation action and restores the
actual trigger on dismissal, even if it was not focused before expansion.

Custom actionable components can opt into the same integration without a wrapper
button or a duplicate accessible node. At the start of `RenderOnce::render`,
capture `popup::PopupTrigger::current(cx)`. After installing the control's own
`ActivationGuard`, call `binding.install(element, &guard, &focus)`. Finally return
`binding.apply(accessibility.apply(element))`. Keep the guard and focus handle in
retained component state. One primary native control claims each popup trigger;
compound triggers should expose their own semantic root. Unavailable native
controls suppress click/context-menu popup activation but retain hover hints.

## Placement and appearance

`PopupPlacement` provides twelve physical placements. RTL changes content
direction without changing these physical names. Centered placements can flip
and shift; edge-aligned placements flip. `PopupArrow` controls visibility and
point-at-center alignment. Geometry follows current trigger bounds during
prepaint, so scrolling, resizing, and content changes do not need polling.

Tooltip and Popover accept preset or custom colors. Tooltip computes foreground
contrast for custom colors. Component token patches use the same `ComponentTheme`
infrastructure as the rest of the library. Semantic refinements include the
surface and each component's title/content, description, icon, and action slots.
Scoped `tooltip`, `popover`, and `popconfirm` defaults remain independent.

Entry uses opacity with reference timing and easing. The user-selected native
adaptation closes immediately: no outgoing content remains in hit testing,
keyboard traversal, or accessibility. Reduced motion settles immediately and
requests no animation frames. Unique tooltips share one owner per configured
scope and window; outgoing content is removed immediately on handoff. The shared
background interpolates position and size over `motion_duration_slow`, while the
new content fades at its final position. `PopupHandle::layout()` reports the
content's final layout, which also owns input and accessibility. Reduced motion
finishes both entry and geometry transitions immediately.

## Gallery and verification

```powershell
cargo run --locked --offline --example gallery -- --component tooltip
cargo run --locked --offline --example gallery -- --component popover
cargo run --locked --offline --example gallery -- --component popconfirm
cargo test --locked --offline --lib popup::
cargo test --locked --offline --example gallery popup::tests::
cargo test --locked --offline --example gallery popup_gallery
./scripts/run-popup-perf.ps1
./scripts/test-popup-accessibility.ps1
```

The gallery includes all 27 public reference examples, an independent edge-preview
window, controlled async completion, failure/retry, scrolling and disabled hints,
nested confirmation, and keyboard focus examples. The [verification report](popup-system.md)
records the browser comparisons, native Windows checks, performance measurements,
and known differences, including RTL paragraph direction and unverified platforms.

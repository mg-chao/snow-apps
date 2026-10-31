# Modal

Native GPUI dialogs following Ant Design 6.6.7, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`. The GPUI revision and runtime dependencies
are unchanged. Dialog content remains native, including text inputs, buttons,
scroll areas and popups; no browser or image of the dialog is embedded.

## Controlled dialogs

Create the controller once in the owning view, retain its `Entity`, and rebuild
`Modal` with a stable ID. `UiRoot` automatically installs the window's host.

```rust,ignore
use std::rc::Rc;
use ant_design_gpui::{Modal, ModalState};
use ant_design_gpui::modal::{ModalAction, ModalContent, ModalOptions};

let modal = cx.new(|cx| {
    let mut state = ModalState::new(window, cx);
    state.set_scope(scope.clone(), cx);
    state.set_options(ModalOptions {
        title: Some(ModalContent::text("Save changes?")),
        content: Some(ModalContent::text("Your changes are ready to save.")),
        on_ok: Some(Rc::new(|_, _, _| ModalAction::Close)),
        on_cancel: Some(Rc::new(|_, _, _| ModalAction::Close)),
        ..Default::default()
    }, cx);
    state
});

// In an application event handler:
modal.update(cx, |state, cx| state.set_open(true, window, cx));

// In render:
Modal::new("save-dialog", &modal, cx).scope(scope.clone())
```

The controller is authoritative. Silent setters do not emit action requests.
Without action handlers, an ordinary Modal emits `OkRequested` or
`CancelRequested(reason)` and stays open until its owner calls `set_open(false)`.
Subscribe through GPUI's `EventEmitter<ModalEvent>` or `on_event`. The cancellation
reason distinguishes button, close icon, mask and Escape.

Opening and closing emit `AfterOpenChange(bool)` when the transition settles.
`AfterClose` follows the false completion. Callbacks may reopen the dialog;
generation checks prevent old cleanup from retiring that new opening. Unmounting
the builder releases its retained content and dependent overlays without inventing
a user cancellation request.

## Managed confirmations

```rust,ignore
use ant_design_gpui::ModalManager;
use ant_design_gpui::modal::{ModalContent, ModalOptions, ModalOutcome};

let manager = ModalManager::new(scope.clone(), window);
let handle = manager.confirm(ModalOptions {
    title: Some(ModalContent::text("Delete these items?")),
    content: Some(ModalContent::text("This operation cannot be undone.")),
    ..Default::default()
}, window, cx);

handle.update(|options| options.ok_text = Some("Delete".into()), cx);
let result = handle.result(cx); // Await in an application-owned task.
// result.await is Confirmed, Cancelled, or Destroyed.
// handle.destroy(window, cx);
// manager.destroy_all(window, cx);
```

`confirm`, `info`, `success`, `warning` and `error` retain the supplied scope.
The host owns their controllers until closing completes. Handles are weak with
respect to controllers but retain the terminal outcome, so a result can be read
after the closed controller is released. Cloned result futures see the same
outcome. `destroy_all` affects only dialogs created by that manager. Managers are
window-bound; do not share controllers or managers between native windows.

Helpers close on OK/Cancel by default. `ModalManager::show` also accepts a complete
`ModalOptions` configuration, including an ordinary dialog. Helper convenience
methods select OK as the initial focus; `initial_focus` and `auto_focus` customize
it. Missing, loading or disabled standard actions fall back to the dialog itself.

## Async actions and loading

An action returns `Close`, `KeepOpen`, or `Pending(Task<Result<(), SharedString>>)`. A
successful task closes its opening; rejection emits `ActionFailed(message)`, keeps
the dialog open, and permits retry. Display the error in application content if
appropriate. The component does not add an unsolicited error dialog.

Dispatch is guarded before invoking callbacks, preventing reentrant submissions.
OK and Cancel have independent pending state. A helper can be cancelled while its
OK task runs. Closing, unmounting or reopening invalidates that task's effect on
the dialog; detached application work still runs to completion.

`set_confirm_loading(true)` is the ordinary controlled Modal's stronger guard:
it shows progress on OK and suppresses all cancellation requests, matching the
reference. Per-button `loading`/`disabled` applies only to that action. Disabling
Cancel does not disable the close icon, mask or Escape.

`options.loading` is body loading. It hides the body and footer and displays the
reference four-row skeleton. Body/footer factories are deferred while it is
loading. The internal native renderer reproduces the 1.4-second, three-stop
gradient with at most a few paint quads per row, using the dialog's single frame
request. Hidden windows, closed dialogs, covered loading dialogs and reduced
motion do not keep a loading animation running.

## Content lifetime and customization

| API | Ownership and behavior |
| --- | --- |
| `ModalContent::text` | Shared string with scoped typography and paragraph bidi direction |
| `ModalContent::render` | Fresh native element factory; receives the handle, window and app |
| `ModalContent::view` | Lazy factory retaining one native view for that slot |
| `ModalFooter::None` | No footer or standard actions |
| `ModalFooter::Content` | Application footer, with the same content lifetime rules |
| `ModalFooter::Render` | Composes fresh native OK/Cancel actions with application content |
| `surface_render` | Wraps the completed native surface, e.g. the gallery's draggable example |

Custom renderers can read `handle.state().unwrap().read(cx).scope()` for inherited
configuration. `behavior::paragraph_text(id, text, direction)` supplies the same
cached native bidi shaping used by text slots and Alert; custom native controls
remain responsible for their own direction and interaction.

Title, body and content-footer view factories are retained independently. Closing
normally preserves them. `destroy_on_hidden` releases the dialog's cached views
after closing; `force_render` eagerly initializes them while the builder is
mounted. Changing a factory's identity replaces that slot's view. Retain factories
instead of constructing new closures every frame when their identity must remain
stable. An externally retained `Entity` is still owned by the application; Modal
cannot destroy the application's other references.

`ModalStyles` refines root, wrapper, mask, container, header, title, body, footer
and close slots. `styles_fn` additionally receives kind, loading, confirmation
loading and direction. `surface_render` accepts and returns `AnyElement`; ordinary
GPUI layout and input handlers provide native interaction without React wrappers.

Use `ModalWidth::Fixed(Pixels/Fraction)` or six responsive breakpoints
(`xs, sm, md, lg, xl, xxl`). Missing responsive entries inherit the preceding one.
The default widths are 520 and 416 logical pixels; both clamp to viewport gutters.
The default top is 100. `centered` centers short content and keeps oversized content
scrollable from its top. Semantic root offsets can reposition a dialog; the gallery
clamps dragging to the viewport.

## Configuration, theme and locale

`ConfigPatch.modal` inherits centering, mask behavior, focus trap/return, button
options and semantic styles. Instance values override scope values. Button option
fields are optional: `Some(false)` explicitly resets an inherited `Some(true)`;
styles refine field by field. The footer and event dispatcher resolve the same
effective button configuration.

`ModalTheme`/`ModalTokenPatch` provide content/header/footer backgrounds, title
typography, padding, borders, gaps and confirmation spacing. Resolution is cached
by the existing theme system. The close button uses a retained child scope to
adapt native Button rest/hover/active colors without rebuilding a theme per frame.
Light, dark, compact, dark-compact and wireframe follow the upstream formulas.

`Locale.modal` supplies `ok_text`, `cancel_text`, and `just_ok_text`. All three
built-in catalogs (`en_US`, `zh_CN`, `zh_TW`) are complete. `LocalePatch.modal`
supports per-field overrides. A live scope update preserves controller, content,
focus and pending action identity. User-supplied titles/content are localized by
the application, not treated as stable identifiers.

## Host, focus and accessibility

`ModalHost` is a window-local, weakly registered overlay owner. Declarative mounts
observe their controller; managed dialogs have host-owned subscriptions. Surfaces
are laid out as roots and deferred above ordinary content. Popups receive the
owning Modal's paint band, so a child popup is above its dialog and below a later
dialog. A nested declarative dialog inherits paint ownership; closing/unmounting
its parent closes that branch. Independent managers do not become parent/child
merely because their dialogs overlap.

Custom roots install `ModalHost` around `PopupHost`, then the content. `UiRoot`
also installs `MessageHost` outside `ModalHost`, so global feedback remains
accessible while a dialog hides background content. Messages and fullscreen Spin
overlays participate in the dialog stack according to their configured z-index.
Keep the hosts alive for the native window's lifetime.

Tab and Shift+Tab wrap inside the top trapped dialog. Child popups receive Escape
before the dialog. Removing a focused control and attempted focus escape fall back
inside the active branch. Closing returns focus to a surviving trigger; `UiRoot`
handles removed focus targets. `focus_trap`, `focus_return`, `keyboard`, `mask`,
`mask_closable` and `scroll_lock` are independent settings. A disabled mask defaults
to no focus trap; it still does not allow pointer activation behind the dialog.
Unlocked wheel input can reach background scrolling without enabling background
clicks. Tall dialogs and child scroll areas consume their own scrolling first.

Mask dismissal requires press and release outside the measured dialog and its
popups. Moving from content onto the mask cannot accidentally dismiss the dialog.
During closing, an input shield retains the pixels but makes the surface inert.

Ordinary surfaces expose `Dialog`; helpers expose `AlertDialog`. The accessible
name defaults to a text title; custom title content can set `accessibility_label`.
Descriptions, modal/busy state and stable automation IDs are supported. A named
background group excludes the covered subtree from the accessibility tree. The
default action IDs are `modal.ok`, `modal.cancel`, `modal.close`, and `modal.dialog`.

## Verification

The gallery exposes all 17 public reference examples plus nested overlays, tall
content and retained/destroyed views. Run `cargo run --locked --offline --example
gallery -- --component modal`. `examples/modal.rs` is the isolated visual surface.

Focused validation commands (no full suite):

```powershell
cargo test --locked --offline --lib -- modal:: root:: popup:: config:: locale:: behavior::text:: alert:: icons:: behavior::tests::semantic_adapter behavior::tests::busy_semantics --test-threads=1
cargo test --locked --offline --example gallery modal::tests -- --test-threads=1
cargo clippy --locked --offline --lib --tests --example modal --example gallery --bench modal -- -D warnings
./scripts/test-modal-accessibility.ps1
./scripts/run-modal-perf.ps1
```

The performance script requires the configured `windows-msvc-performance` tree,
checks benchmarks are enabled with Release, and records executable/source hashes.
It measures cached tokens, 100 closed dialogs, a 20-row dialog, redraws,
close/reopen cycles, resizing and six stacked surfaces. It asserts zero lazy
construction while closed, retained content reuse, zero settled animation frames
and managed-controller release. CPU timings do not measure GPU presentation or
end-to-end input latency.

The independent reference imports the original React component, its pinned
dialog dependency and upstream `reset.css`. No native geometry is used to generate
the reference. `fixtures/modal/reference.json` contains computed DOM metrics and
source/package provenance. Reproduce captures with:

```powershell
node scripts/prepare_modal_reference.mjs --install true
node scripts/capture_modal_reference.mjs --cases fixtures/modal/visual-cases.json --output ../build/ant-design-gpui/modal-reference/live-100 --scale 1
# Repeat for live-150/1.5 and live-200/2.
python scripts/pack_modal_reference.py
cargo build --locked --offline --example modal
./scripts/capture_modal_native.ps1
python scripts/compare_modal_visual.py
```

The capture harness sends DPI changes only to its own hidden Windows window and
checks actual platform scale. Thirty cases cover every kind, the four algorithms,
both directions, wireframe, loading, custom tokens, narrow/wide widths, title/footer
omission, centering and tall content at 100%, 150% and 200%. Browser animations are
paused at frame zero for reproducibility. Comparisons retain complete unaligned
canvases, geometry differences, solid-color probes, helper icon bounds and image hashes in
[`visual-validation-windows.json`](../fixtures/modal/visual-validation-windows.json).

### Windows results, 2026-10-28

- **183 focused library tests** passed, covering Modal, root/popup integration,
  configuration and locale, shared bidi text, Alert regressions, icons and affected
  accessibility semantics. **Two gallery tests** passed: all 20 examples mount/open,
  and native pointer dragging moves, clamps and stops on release.
- Strict Clippy (`-D warnings`), scoped rustfmt and whitespace checks passed.
  No full crate or repository test suite was run.
- **90 independent browser/native comparisons** passed (30 cases × three DPI scales).
  Dialog bounds matched exactly; all sampled solid colors matched exactly. Helper
  icon outlines differed by at most **0.667 logical pixels**, measured at 50% color
  coverage. Fully opaque icon-core edges differed by up to **1.333 logical pixels**
  at 150% because of native SVG antialiasing; that diagnostic is retained separately.
  Mean absolute channel error over the complete, unmasked frames was **0.554/255**.
  Side-by-side review confirmed the corrected RTL punctuation, action alignment,
  status icons, loading rows and themed surfaces. Font and shadow rasterization
  differences remain visible in the original images.
- **Eight native Windows UI Automation checks** passed: title/role/background
  isolation, Tab containment, Cancel/focus return, named Close, async busy/completion,
  Traditional Chinese helper semantics, disabled Cancel and normal window teardown.
  See [`accessibility-windows.json`](../fixtures/modal/accessibility-windows.json).

Release CPU/layout results used the configured `windows-msvc-performance` tree
on an AMD Ryzen 9 5950X (Rust 1.98.1). Values include the test window's native
layout/paint recording work, with warmed caches and motion disabled.

| Operation | Mean | p95 |
| --- | ---: | ---: |
| 100 closed dialogs, retained draw | 0.476 ms | 0.661 ms |
| 100 closed dialogs, invalidated | 0.487 ms | 0.690 ms |
| One open dialog, 20 rows, retained draw | 0.292 ms | 0.377 ms |
| One open dialog, 20 rows, invalidated | 0.329 ms | 0.485 ms |
| Retained close/reopen | 1.026 ms | 1.410 ms |
| Resize | 0.282 ms | 0.482 ms |
| Six stacked dialogs | 0.892 ms | 1.002 ms |

Cached token lookup batches of 100 averaged 1.689 microseconds. Assertions verified
zero closed lazy constructions, one retained body construction across reopening,
zero settled frame requests and release of all five managed helper controllers.
Sources were unchanged during the benchmark. Timings and content-addressed
provenance are saved in [`performance-windows.json`](../fixtures/modal/performance-windows.json)
and [`performance-windows.txt`](../fixtures/modal/performance-windows.txt).
The complete feature-to-test map and evidence hashes are in
[`verification.json`](../fixtures/modal/verification.json).

### Deliberate differences and verification limits

- The pinned GPUI API has subtree opacity but no general subtree scale or backdrop
  blur. Transitions fade; the mask dims. Browser zoom/origin and blur are not
  emulated with screenshots. The gallery states this difference.
- Native font/SVG antialiasing and shadow rasterization differ from Chromium.
  Geometry and solid colors are compared separately from complete-image pixel error.
- Reduced motion and hidden windows stop the skeleton, even though the browser's
  active skeleton may animate with theme motion disabled.
- Browser DOM portals, CSS class names and deprecated React aliases have native
  equivalents instead of literal API ports. The manager replaces static global
  functions with explicit window/scope ownership.
- Native Windows behavior and rendering are verified. Native macOS/Linux execution,
  real screen-reader speech output, GPU frame-time and end-to-end latency are not
  verified by these checks.

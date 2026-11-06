# Popup ownership and memory

Popup content is constructed only for a presentation. Existing exit animations
retain their outgoing resources until completion; fully hidden or unmounted
surfaces release their content, popup-only controllers, subscriptions, timers,
and rendering work. Opening again uses current application configuration.

## Migration

Remove popup `fresh` and `destroy_on_hidden` calls, ColorPicker's
`destroy_on_hidden`, ModalOptions `force_render`/`destroy_on_hidden`, and Menu
`force_submenu_render`. These options no longer exist. Tab-pane lifecycle options
are unrelated and retain their existing behavior. `ModalContent::view` remains a
lazy view factory, with identity stable during one presentation.

Keep selected values and editable application data in controller/model entities,
not disposable view-local state. Factories should allocate heavyweight content
inside their body and reference application owners weakly. Modal retains its
configuration factories because imperative reopening must reconstruct content;
those factories must not close over complete popup views. Application-owned
entities, image source collections, callbacks, and explicit export caches remain
application-owned even when another reference is passed to a popup.

## Ownership ledger

| Surface | Retained while hidden | Released when fully hidden |
| --- | --- | --- |
| Anchored popup / Tooltip / Popover | Trigger identity, scope, visibility state/opening callback, and scalar focus-resume/return-target intent | Content factory, surface focus, layout, surface callbacks and child keyed state |
| Popconfirm | Weak handle and request/session scalars; action definitions belong to the caller | Action focus, pending presentation state and content; independent application tasks continue |
| Select / Pagination size Select | Options, values, search, stable native input; one scalar reopening record | Filter/order snapshots, virtual-list height trees and popup layout |
| DatePicker / TimePicker | Trigger editors, accepted value, drafts, range/panel selection and scalar panel focus route | Panel/column focus, scroll handles and debounce work |
| ColorPicker | Accepted value, mode/format, stop IDs, one previous-gradient restore snapshot, scalar focus target and five current text/selection pairs | Six editor controllers, subscriptions, nested Select, panel focus/geometry and interaction snapshots |
| Menu | Item model, selected/open keys and inline navigation; at most 64 scalar branch scroll/active records, LRU | Popup rows, overflow subsets, list/metric snapshots, branch focus and hover work |
| ContextMenu | Root items, check/radio values and one scalar active index | Presentation levels, lazy branch snapshots, lists, focus, scrolling and metrics |
| Tabs More | Tab model, selected tab, pane state and one scalar reopening record | Overflow focus, scrolling, rows and captured popup snapshots |
| Modal | Options, root focus, weak handles and manager outcomes | Title/body/footer views, scroll controller, action/sentinel focus and close-button scope |
| Image preview | Items/registrations, current index, options, scalar focus-resume intent, weak return target and stable empty picture controller | Dialog shell, operation focus, picture source and fallback, pixels, measurements and animation wakeups |
| Message / Notification | Window service configuration, live notice definitions/countdown budgets and completion outcomes | Finished notice descriptions, timers, visual maps and burst collection capacity |
| Fullscreen Spin | Loading intent, logical delay deadline and transition scalars | Hidden indicator subtree, animation wakeups and paint geometry |

Color draft snapshots contain current strings and selections only, with no undo
history or editor reference, and are validated against current color/format. Reset,
unmount, and incompatible external updates discard them. Focus resumes only for
surfaces that owned it before native hiding; pointer-only hints do not steal focus.

Pagination and Segmented native hint factories capture only text and scope. Their
views are created after the native delay and released on hide. Their mounted
visibility observers also clean up views/timers in custom roots without UiRoot.

Inline picker panels keep their presentation resources while mounted. Ordinary
menu rows, tab panes, trigger editors, and caller-owned models are not popup caches.
Controlled close requests that are rejected keep the current presentation intact.
Unique-tooltip replacement and clipping can hide a presentation without changing
its authoritative open value.

## Rendering leases and bounded metadata

Shared scene leases cover cached native scene replay. Pending decode, upload,
orientation, and loading-mask preparation receive leases before returning Pending.
Last-user release cancels internal work and retires native images safely; a late
completion cannot resurrect a hidden presentation. Application confirmation tasks
remain independent and use generation checks before applying results.

Weak lookup limits are 64 popup shadows, 128 decoded sources, 32 GPU textures,
32 oriented rasters, and 16 loading masks. LRU eviction discards lookup metadata;
active scene leases remain valid. No heavy idle payload is retained by these
registries. Shadow, icon, decoder, GPU, transformed-raster and loading-mask consumers
first recover their own scene slot, validating its full key before consulting the
weak index. Exceeding an index's capacity cannot restart an existing consumer's
pending work or replace its ready resource. Fingerprint collisions replace only
that scene slot and defer obsolete-resource retirement until cache borrows have
ended. Failed replacement preparations release the obsolete resource and leave
an empty slot that can be retried.
Painted two-tone and full-color icons also release their cache-owned raster and
parsed-SVG references after acquiring a scene lease; explicit headless/export cache ownership
remains under IconRenderer's existing limits. The icon weak lookup is capped at
512 entries. GPUI's platform text/SVG atlas policies are unchanged.

Lightweight reopening caches are four calendar grids, 128 date labels, and
32 color-geometry entries under a 256 KiB storage budget, all LRU. Oversized
color geometry is rendered without caching. Inactive unique-tooltip geometry retains at
most 64 entries per window, LRU with the existing handoff expiration. Caches own
no popup entities or content factories. Empty dynamic collections release their
historical allocation capacity. Notice focus coordination keeps weak active
boundaries only and unregisters/shrinks immediately as boundaries drop.

Native window hiding suspends presentation resources and performs one coalesced
cleanup draw without presenting it, retiring cached scene leases even when no
platform frame arrives. Restoring the window rebuilds visible presentations.
Exit deadlines use cancelable weak tasks, independent of animation frame delivery.

## Validation

Deterministic lifetime regressions cover never-opened controllers, exit completion,
reopening, controlled rejection, cached parents, nested content, unmount, and native
window visibility. Weak/drop probes and live raster statistics distinguish live
resources from total allocation churn. Only affected library/gallery selections
and related accessibility/performance fixtures are run; the full suite is excluded.

Executed commands, scoped test results, native accessibility/capture checks and
Release measurements are recorded in [VALIDATION.md](../VALIDATION.md#popup-memory-lifecycle--2026-11-06).
Native checks cover Windows; macOS/Linux and real screen-reader speech remain
unverified. Resource measurements use live counters and drop probes, not RSS or
physical VRAM measurements.

### Lifetime acceptance measurements

The baseline column is the audited ownership before this change; assertions use
resource identities/drop probes, not process RSS or allocator total-call counts.

| Scenario | Audited baseline | Verified post-refactor result |
| --- | --- | --- |
| Never-opened ColorPicker | Six dedicated editor controllers plus subscriptions | Zero panel editor controllers |
| Never-opened Modal | One scroll controller and five action/sentinel focus handles | No presentation bundle |
| Never-opened preview | One Modal shell and nine operation focus handles; closed changes could assign pixels | No shell/operation handles; stable picture has no source/fallback/image |
| Hidden variable Select | Row-height tree and filtered/order snapshots retained | No variable ListState or derived row snapshot |
| Closed Menu branches | Rows, metrics, focus and virtual lists accumulated until model replacement | No branch payload; at most 64 scalar LRU records |
| Last hidden popup shadow user | Idle LRU could retain 64 rasters / 16 MiB | Zero live rasters / zero raster bytes after scene retirement |
| Shared pending decoded source | Departed waiter IDs accumulated until completion | Only currently leased waiter IDs; final-user cancellation |
| Finished notice burst | Peak Vec/HashMap backing allocations remained | Empty storage capacity released; sparse live maps shrink |

Release measurements show the expected cost of releasing idle assets: popup
close/reopen recreating its shadow averaged 13.388 ms (p95 17.932 ms), while
warm alignment averaged 0.241 ms (p95 0.328 ms). The historical retained-shadow
reopen report averaged about 0.298 ms; it is not a controlled same-machine
comparison. Hidden shadow payloads now measure zero rasters and zero bytes after
scene retirement. Per-component benchmark results and source/executable hashes
are in the validation report.

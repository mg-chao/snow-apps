# Persistent screenshot toolbar popover investigation

## Confirmed pinned-toolbar recurrence, 2026-09-12

Live debugging of PID 69252 established a mixed-DPI hover geometry defect. Mouse
enter events reached multiple popup controllers, but no hover-open callback fired.
The controllers were enabled and closed; Qt reported no pressed mouse buttons,
explicit mouse grabber, or pressed-button owner. The GUI thread was in its normal
event loop.

At `screenshotActionToolGroupButton0`, the real cursor was around `(879, 1100)` and
the button mapped to `(860, 1081)-(891, 1112)`. However, its ancestor
`ScreenshotToolPaletteHost` mapped to `(-120, 1580)-(1121, 1721)`. The hover helper
mapped each ancestor's origin independently, then intersected those global rectangles.
Qt's mapping chooses a screen-dependent scale for each point when a window spans
screens. Here the ancestor origin and button fell on different display scales, so
the intersection became empty despite valid local containment.

`triggerContainsGlobalPos()` consequently always returned false, preventing the
outside-to-inside transition and hover-open scheduling. Main and sub-tool popovers
share this path. The Table request failures in the same session do not establish
a causal connection to this recurrence.

The initial diagnostic correction reused `widgetVisibleRectInAncestorMappedToGlobal()`,
which popup placement already used: clip through ancestors in scope-local coordinates, then map
the surviving rectangle once. Redirecting only that call in the existing process
restored both main and sub-tool popovers, as confirmed by the user. No restart,
toolbar recreation, or timer reset was needed. Debugger breakpoints were removed
and the debugger detached; the corrected call remains in that process until exit.

`adqt-popup-hover-geometry-tests` configures offscreen 100% and 150% displays and
recreates the disjoint global rectangles. It checks main/sub-tool hover opening,
sibling reopening, actual surface visibility, and rejection of a locally clipped
button. This test is independent of the machine's physical display arrangement.

Validation: the new regression fails at hover opening with the original helper and
passes with the correction. The related popup hover recovery and Snow Shot toolbar
popup recovery tests also pass after rebuilding. Changed C++ files pass clang-format,
and `git diff --check` passes.
The full suite was not run. The running application retains the diagnostic correction;
the on-disk application executable still needs a normal rebuild after that instance exits.

### Structural correction and related-path audit

The initial live correction proved the ancestor-intersection cause, but still
constructed a global rectangle from one mapped point plus an unchanged local size.
That representation also fails when a control itself spans a scale boundary.
The permanent implementation therefore replaces the geometry mechanism rather than
retaining the one-call diagnostic patch.

`widgets/detail/popup_geometry.{h,cpp}` defines two explicit representations:

- `PopupWidgetRect` retains the widget owning its local rectangle. Visibility clips
  against ancestors in local coordinates and stops at the widget's own window.
  A top-level window's QObject owner is not a clipping ancestor. Hit testing maps
  the global pointer back into the owning widget before testing containment.
- `PopupScreenRect` is a placement snapshot with a guarded screen pointer. Projection
  maps one reference point and scales the rectangle's offsets and size into that
  screen's coordinates. It must never be used as a desktop hit-test region.
  Cross-widget mapping stays local within a window; across windows it maps one
  reference point and preserves physical size using the windows' DPR ratio.

`OverlayPopupController::resolvedAnchorRect()` now serves both layout synchronization
and its cached-layout fast path. Tooltip delegate overrides provide local rectangles;
transient tooltips retain explicit screen snapshots until an anchor update. The
controller validates visibility locally, eliminating global ancestor intersections.

The same invariant is applied to:

| Path | Defect addressed |
| --- | --- |
| Main/sub-tool hover and controller ownership | Valid visible controls rejected by global rectangles |
| Popup interaction host | In-scope outside presses missed; nested anchors classified using a false global center |
| Tooltip bridge and custom trigger/anchor rectangles | Cross-window size mismatch and comparisons in different screen coordinate spaces |
| Select placement and select/menu ownership | Incorrect anchor extents and hover/press containment at a scale boundary |
| Isolated busy indicator | Small overlay positioned with local dimensions in another screen's units |
| Date-range popup arrow | Subtracting independently mapped global points from different screen spaces |
| Spotlight opacity wheel handler | Wheel input rejected even though the pointer is inside the slider |

The existing popup surface body hit test and Snow Shot floating-toolbar interaction
region already map the pointer into local coordinates. Navigation-menu placement
already works in scope-local coordinates. These paths required no geometry redesign.
Modal centering uses the owner's native frame geometry in its normal path; the
remaining fallback expression does not establish another reproduced failure.
Single-point QMenu placement is outside this rectangle-clipping defect.

The expanded offscreen regression covers both source screen scales, controls crossing
the boundary, local clipping, hidden ancestors, owned tool windows, nested dismissal,
select/menu ownership, tooltip sub-rectangles, small overlay alignment, and date-range
arrow alignment, in addition to the original main/sub/main reopening scenario. It
also checks frozen transient anchors, explicit recapture, and recovery when an
initially clipped anchor returns to view. Invalid placements are not captured.
A negative control restoring only the old mapped-origin-plus-local-size hit test
fails with `both sides of a control spanning different display scales must be interactive`.
The corrected implementation passes. The original ancestor-intersection regression
also failed before the initial correction and passed afterward.

Final validation uses the Debug build and only relevant tests:

- `adqt-popup-hover-geometry-tests` (deterministic simulated 100%/150% displays).
- `adqt-popup-hover-recovery-tests`, `adqt-select-popup-lifetime-tests`,
  `adqt-select-tests`, `snow-shot-toolbar-popup-recovery-tests`, and
  `snow-shot-spotlight-wheel-tests`.
- `adqt-qt-tool-popup-tests -platform offscreen`: 17 passed, no failures or skips.
- Changed C++ files pass `clang-format --dry-run --Werror`; `git diff --check`
  passes. The preset has `SNOW_APPS_ENABLE_CLANG_TIDY=OFF`, so no clang-tidy run
  is claimed. No full repository test suite was run.

The Windows-only busy indicator uses the tested small-overlay projection; its native
compositing was not exercised by the offscreen regression. These tests establish the
coordinate invariant and affected behavior, not every physical monitor arrangement
or native compositor interaction. The running Snow Shot instance still contains
only the earlier diagnostic call patch; the permanent refactor is in source and the
rebuilt test targets, pending a normal application rebuild after that instance exits.

The older investigation below describes a separate, previously unconfirmed failure.
Its logging instructions have an additional limitation: `DiagnosticsService::record()`
unconditionally discards `QtDebugMsg`, so `adqt.popup` debug events are not persisted
in Snow Shot's log files even when `QT_LOGGING_RULES` enables the category.

## Earlier investigation

Inspected revision: `1f21a041`, including comparison with `bd994b5c`.

## Observed symptom and conclusion

The user observed the failure before/at `bd994b5c`, after using Table recognition.
Ordinary toolbar buttons still responded; popovers remained unavailable after ending
the capture. The behavior on sub-toolbar popovers is unknown. The user has not reported
a reproduction on the latest commit.

The latest commit fixes a real deadline-callback cancellation/lifetime defect and moves
Table image preparation off the GUI thread. Those changes are relevant, but neither
source inspection nor the tests establish the cause of the persistent all-group failure.
This investigation adds diagnostic coverage, not a speculative popup behavior change.

## Execution path and shared state

1. `createScreenshotToolPaletteOptionPopoverShell()` creates each main group with
   `AdPopover::Trigger::Hover` and `PopupLayerMode::QtTool`. Clicking its trigger executes
   the selected tool; it is not an alternative way to open the menu.
2. `ScreenshotToolPalette::eventFilter()` materializes lazy group content on `Enter` or
   `HoverEnter`. `OverlayPopupController` observes the trigger and its descendants.
3. `reconcileHoverFromCursor()` compares `QCursor::pos()` against the effectively visible
   trigger rectangle and popup body. An outside-to-inside transition schedules the open.
   The default open delay is 100 ms, and active hover sessions use a 25 ms cursor monitor.
4. Every controller uses the same `TimingHub` deadline timer. `finishHoverOpen()` checks
   the cursor again, sets its hover-open reason, and requests visibility.
5. `PopupInteractionHost` coordinates open owners per toolbar window. Showing a sibling
   closes the previous owner unless the new anchor lies inside its interaction region.
6. Geometry is checked against visible ancestors and the target screen; the popup's
   native window is created, assigned a transient parent, shown, and raised.
7. Closing clears hover tasks. Retained QtTool surfaces release their native windows
   through a queued callback, guarded against an intervening reopen.

The toolbar is reused across captures. `ScreenshotOverlayUiHost::resetToolbarForNewCapture()`
calls `ScreenshotToolbarWindow::resetForNewCapture()`. `releaseNativeSurface()` destroys
native resources while preserving the QObject tree. Consequently, surviving a capture
restart does **not** establish that the defect must be an application-global singleton:
the same controller and host objects can survive too.

Sub-toolbar option popovers use the same controller, timer service, and QtTool mechanism.
Color-picker popovers also ultimately use AdPopover, although their activation path differs.
Testing them during a recurrence would distinguish a main-group initialization issue from
a failure in the shared popup infrastructure.

## Findings and remaining candidates

### Confirmed previous defect: overdue callbacks escaped cancellation

Before `1f21a041`, `TimingHub::dispatchDueTasks()` removed all due tasks from its maps and
copied bare callbacks into a vector before invoking any of them. A first callback could
close a popup, cancel a later callback, replace its task, or destroy its QObject context;
the already-copied later callback would still execute. The callbacks commonly capture
raw `this` pointers. This violated both cancellation and lifetime guarantees.

Synchronous Table image encoding on the GUI thread made batches of overdue callbacks
more likely. The current implementation snapshots IDs and checks each still-registered
task immediately before execution, so earlier callbacks can cancel or replace later ones.
The timer test covers those invariants. This establishes a real defect and a relevant
correction, but does not demonstrate that it produced the user's persistent failure.
An encoding stall alone cannot explain popovers remaining unavailable once normal GUI
interaction resumes.

### Confirmed coverage gap: Table test bypassed hover

`tableBusyStatePreservesSiblingGroupPopovers()` previously called `popup->show()`.
It proved content and surface availability, but skipped input delivery, hover state,
and the opening timer. It now leaves group content lazy, sends a hover-enter event at
the actual trigger position, and waits for visibility, checking both the logical flag
and actual widget surface.
It exercises recognition and drawing groups before, during, and after Table busy state,
then after returning to Select. This is a deterministic hover integration check; it is
not an end-to-end reproduction of Table recognition or native mouse routing.

The additional native/offscreen scope-recreation test keeps the same two controllers,
ends the scope both with a pending hover and with an open popup, destroys the native
scope window, recreates it, and opens both siblings by mouse hover again.

### Input delivery or a stalled shared deadline service: still plausible

Normal buttons respond to press/release while group menus need hover transitions and
deadline callbacks. Therefore, working ordinary buttons do not rule these out. The new
trace distinguishes trigger entry, scheduled open, fired open, and surface visibility.
`QWidget::mouseGrabber()` only reports explicit widget grabs; a null value does not
by itself rule out Qt/native implicit mouse capture. No stuck grab or stalled timer
was reproduced here.

### Logical visibility can differ from actual visibility

`AdPopover::isVisible()` reports the controller's `popupVisible_`, not the surface's
visibility. The controller sets that flag and registers its host before geometry is
validated. A first open with a hidden/clipped anchor can therefore report logical
visibility while no surface is shown. The shared host checks actual visibility for
some operations. Geometry/scope diagnostics must be examined before calling such a
case a hover failure. This does not by itself demonstrate a persistent sibling lockout.

### Lazy initialization has a limited fallback

Main groups materialize content only on `Enter`/`HoverEnter`. Their additional
`visibilityRequested` connection is ineffective in the default Automatic visibility
policy; that signal is emitted by the Manual/External path, and empty popovers also
suppress opening. `MouseMove` can recover a controller's hover state without materializing
an empty group. If enter events are lost, the supposed fallback does not provide recovery.
This is a narrower candidate: by itself it does not explain already-materialized sibling
groups remaining unavailable after ordinary pointer re-entry.

Table busy state itself does not disable all sibling groups. Inspection and the focused
test confirm that recognition availability is handled on its own triggers/options.

## Trace a recurrence

Build the Debug application with the changes in this investigation. Fully exit the
previous Snow Shot instance, then launch the Debug executable with the logging rule:

```powershell
$previousLoggingRules = $env:QT_LOGGING_RULES
try {
    $env:QT_LOGGING_RULES = 'adqt.popup.debug=true'
    & './build/windows-msvc-debug/snow_shot/Debug/snow_shot.exe'
} finally {
    $env:QT_LOGGING_RULES = $previousLoggingRules
}
```

Snow Shot's diagnostics service currently discards these debug messages; a debugger
or console capture is required instead of relying on the storage `logs` folder.
The rule must reach the process that actually handles captures.
No change is made to the user's persistent logging configuration.

The added events are `hover.open_fired` and `hover.close_fired`, complementing the
existing `trigger.enter` and scheduling events.
State snapshots include disabled/content availability, hover-inside/open/pending/monitor
flags, the cursor, mouse buttons, explicit grabber, active Qt popup, and active modal widget.
These traces remain opt-in; there is no per-mouse-move or per-monitor-tick logging.

| Trace at the failing group | Investigation branch |
| --- | --- |
| No `trigger.enter` when leaving and re-entering the button | Input routing, capture, covering window, or event-filter delivery |
| `trigger.enter` but no `hover.open_deadline` | Disabled state, stale hover-inside state, trigger visibility/clipping, or cursor mapping |
| Open deadline but no fired event after the cursor remains inside | Check intervening close/cancellation first; then the shared deadline service |
| Fired event but no opening transition | Cursor has left, content is missing, disabled state, or logical visibility already true |
| `geometry.rejected`, detail 2 | Anchor or ancestor hidden/clipped; compare toolbar and anchor rectangles |
| `surface.show` followed by `host.close` | Inspect close reason: 0 outside press, 1 Escape, 2 scope hidden, 3 deactivation, 4 owner hidden, 5 owner destroyed, 6 superseded, 7 explicit |
| Logical and surface visibility true, yet nothing visible | Native exposure, stacking, transient owner, screen, or rendering; the QWidget flags alone do not prove on-screen pixels |

During the failure, compare one already-used group, an unused group, and a sub-toolbar
option popover. Preserve the trace from the successful Table invocation through the
first failure and one capture restart; later failed hovers alone may omit the trigger.

## Focused validation

Only popup/timer/toolbar tests are relevant; do not run the full suite.

```powershell
ctest --preset test-windows-msvc-debug -R '^(adqt-timing-hub-tests|adqt-popup-hover-recovery-tests|snow-shot-toolbar-popup-recovery-tests)$' --output-on-failure
& './build/windows-msvc-debug/ant_design_qt/Debug/adqt-qt-tool-popup-tests.exe' siblingPopoversReopenAfterOverdueHoverTasks siblingPopoversReopenAfterScopeRecreation popoverReleasesAndRecreatesNativeResources -platform windows -o build/popover-investigation-native.txt,txt
```

Passing these checks demonstrates the covered transitions, not reproduction or elimination
of the original persistent all-popover failure.

Validation in this workspace on 2026-09-11: the Debug application and affected test targets
build successfully. All three filtered CTest entries pass, including lazy first-hover
materialization in the Table test. All three listed native Windows QtTest functions pass.
Changed-code clang-format checks and `git diff --check` pass. The full test suite was not run.

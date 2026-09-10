# Persistent screenshot toolbar popover investigation

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

Snow Shot's diagnostics service captures these messages in its configured storage
directory's `logs` folder. The rule must reach the process that actually handles captures.
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

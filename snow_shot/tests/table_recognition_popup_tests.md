# Table preparation and popup recovery

Table preparation returns a request token before image compression. The 35-second deadline
includes worker queueing and image preparation. Cancelling does not interrupt an active codec;
its eventual result is discarded. The network contract remains WebP quality 75, maximum side 2880.

## Focused checks

Build Debug targets `adqt-timing-hub-tests`, `snow-shot-api-client-tests`,
`snow-shot-recognition-session-controller-tests`, `snow-shot-screenshot-tool-palette-tests`,
and `adqt-qt-tool-popup-tests`.
Run CTest with this explicit filter (never the full suite):

```powershell
ctest --preset test-windows-msvc-debug -R '^(adqt-timing-hub-tests|snow-shot-api-client-tests|snow-shot-table-recognition-session-tests|adqt-popup-hover-recovery-tests|snow-shot-toolbar-popup-recovery-tests)$' --output-on-failure
```

The toolbar test invokes the real Table group option, updates its busy state, and checks both
recognition and drawing group surfaces before/during/after loading and returning to Select.
The session test gates preparation on a worker and checks busy state, duplicate activation,
cancellation, changed targets/modes, failure, success, and actual sibling popup surfaces.
The timer test makes all original callbacks overdue before dispatch, then destroys, cancels,
or replaces a later task from an earlier task. The popup test exercises overdue hover tasks.
For native window recreation and mouse hover, run these QtTest functions with `-platform windows`:
`siblingPopoversReopenAfterOverdueHoverTasks` and `popoverReleasesAndRecreatesNativeResources`.

## Local reproduction diagnostics

Set `$env:QT_LOGGING_RULES='adqt.popup.debug=true'` in the launching shell, then start the current
Snow Shot build. Open a capture, open a grouped toolbar popover, invoke Table, and move among
sibling groups during preparation and after success/failure. Repeat after changing the selection
and after cancelling capture. Check both drawing groups and recognition groups.

The existing Snow Shot logs capture `adqt.popup` transitions. Geometry rejection details are:
1 = missing in-window parent, 2 = hidden/clipped anchor, 3 = component bounds rejection.
Messages include requested/actual visibility, anchor/scope rectangles, cursor/buttons/grabber,
and native creation/release. No per-monitor-tick logging is enabled. Remove the shell environment
variable after reproduction to disable this opt-in trace.

`table.source_prepared` measures screenshot composition. `table.image_prepared` records worker
queueing and preparation, linked by operation ID to `request.finished`; its duration now includes
preparation, with transport time reported separately. No image or recognized text is logged.

## Performance

Build `snow-shot-table-preparation-benchmark` with `windows-msvc-performance`, then run only that
Release executable. It compares the old synchronous preparation path with asynchronous API
submission and the next GUI-thread event, using the same deterministic 2880x1620 noisy image.
This is a stress input, not a representative screenshot or an end-to-end service benchmark.
There are no hardware-dependent CI thresholds. Debug and packaging presets are not benchmarks.

Passing these regressions establishes responsiveness and covered lifecycle behavior; it does not
by itself prove the original persistent all-popover failure has been reproduced or eliminated.

## Validation in this workspace

The Debug application builds. Timer/API/session/hover tests pass, and the two targeted native
Windows popup tests pass. The original persistent all-popover failure has not been reproduced.
The performance Release benchmark builds, but the installed Release `heif.dll` fails initialization
with Windows error 1114 (also reproducible by loading that DLL independently), preventing the
codec backend and benchmark from reaching main. No Release timings are claimed from that run.

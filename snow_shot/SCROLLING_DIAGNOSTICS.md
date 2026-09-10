# Scrolling screenshot diagnostics

These events are included in normal diagnostic exports; no debug environment variables are
required. Reproduce the issue, leave scrolling mode open for at least five seconds if the preview
does not appear, then exit scrolling mode and export the log. Include the reproduction time and
whether physical mouse-wheel input, automatic scrolling, or both failed.

Group events by application `session` and scrolling `fields.operation`. Source segments can
restart under the same operation when an export is paused/resumed. A direction change advances
the operation. Exclusion restoration retains the operation that installed the exclusion.

## Startup and input

| Event | Interpretation |
| --- | --- |
| `scrolling.start_rejected` | Selection or display prerequisites were missing; `reason` identifies the check. |
| `scrolling.window_exclusion` | Capture exclusion requested/restored; `status` is the requested exclusion state, `outcome` is success/failure, and `code` is the Windows error. Failure with code zero can mean an unsupported/precondition failure rather than a Windows API error. |
| `scrolling.started` / `scrolling.preparing` | Controller entered scrolling mode. Includes canvas/physical selection, recognition mode, and color restoration. Does not establish source readiness. |
| `scrolling.input_state` | Per-display overlay rectangle, local input hole, DPR, Qt mask state, full-display-hole flag, and thumbnail visibility. On Windows also includes native region type, whether that region covers the intended hole center, capture ownership, inactive-window wheel routing, and display affinity in `code`. |
| `scrolling.input_target` | Whether the physical selection center hits a window belonging to Snow Shot, and whether Snow Shot owns the foreground window. A single-point sample is evidence, not proof that the entire selection is blocked. |
| `scrolling.prepared` | Synchronous overlay preparation returned; native capture is still asynchronous. |

`full_hole=true`, `mask_empty=true`, and `thumbnail_visible=false` expose the case where a full
display selection has no applied mask. `native_hole_contains_center=true` indicates that the
native window region still includes the sampled hole center. Windows native region type zero
means `GetWindowRgn` returned `ERROR` (including no explicit region); it does not by itself prove
a native API failure. Geometry is metadata only; no screenshot pixels or window titles are logged.

## Capture and preview

| Event | Interpretation |
| --- | --- |
| `scrolling.source_initializing` | Capture producer reached source preparation; viewport dimensions are recorded. |
| `scrolling.native_create` | Native stream creation is about to run. Backend is the requested policy, currently Auto; mode is the WGC update mode. |
| `scrolling.source_ready` | Source/pool initialization returned successfully; duration includes source initialization. |
| `scrolling.backend_selected` | A native frame reports a backend for the first time or a backend change. Values: 0 unknown/Auto, 1 DXGI, 2 WGC, 3 GDI. This is the frame's backend, not a log of every failed fallback attempt. |
| `scrolling.first_frame` | First frame reached the consumer, including dimensions, Qt image format, stride, and duplicate flag. It may still be rejected. |
| `scrolling.frame_rejected` | First structurally invalid frame in the source segment, with actual/expected dimensions, Qt image format, and stride. |
| `scrolling.stitch_result` | First completed stitch result, or a fatal result; includes native stitch event, changed/fatal flags, and output dimensions. |
| `scrolling.first_preview` | Preview was applied to the thumbnail. Includes elapsed time and current overlay/thumbnail state. |
| `scrolling.preview_timeout` | No initial preview after five seconds of UI event-loop time. Input state is sampled again. This is diagnostic only; it does not cancel, retry, or change backend. |
| `scrolling.failed` | Controller accepted a capture/stitch failure; the adjacent existing warning contains its error message. |

`scrolling.capture_progress` reports consumer counters after five seconds and at most once every
30 seconds afterward. `scrolling.capture_summary` reports final counters when the consumer exits:
received/accepted frames, receive timeouts, duplicate/invalid frames, mailbox drops, unavailable
pool buffers, and native dropped-frame events. Dropped events count notifications, not frames.
An accepted frame was admitted to the stitching mailbox; it is not necessarily a rendered preview.
A receive timeout means no queued event arrived, not that a backend returned a fallback error.

If initialization is blocked, source readiness will be absent. If frames arrive but are discarded,
the counters identify why. If accepted frames exist but no stitch result arrives, investigate the
stitch worker. If a preview arrives but is invisible, inspect overlay and thumbnail state.

The UI watchdog cannot fire while the UI thread is blocked. The last emitted preparation event
then helps narrow the location. Backend attempt/failure details before the first frame remain in
native error messages when those errors propagate; the selected-backend event alone cannot prove
why a previous backend was skipped.

## Automatic scrolling and lifecycle

`scrolling.auto_scroll` records enable/disable requests. `scrolling.wheel_dispatch` records the
first dispatch result and changes in status/error; status values are:

| Status | Meaning |
| --- | --- |
| 0 | Wheel message posted successfully; does not prove the application handled it. |
| 1 | Empty selection or wheel delta. |
| 2 | No eligible target window at the selection center. |
| 3 | Screen-to-client conversion failed; inspect Windows error `code`. |
| 4 | `PostMessageW` failed; inspect Windows error `code` (for example, access denied). |
| 5 | Unsupported platform. |

`scrolling.mode_changed` and `scrolling.export_pause` explain intentional pipeline transitions.
`scrolling.stopped` includes whether an initial preview arrived, the latest output dimensions,
elapsed time from the current preview watch, and whether screenshot presentation will be restored.
It does not distinguish every caller's cancellation/export reason.

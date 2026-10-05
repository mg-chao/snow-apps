# Capture exclusion

Snow Shot keeps capture controls visible while omitting them from captured display
or region pixels. The scrolling overlay and toolbar follow the scrolling UI
capture setting; the recording toolbar follows the recording toolbar setting.
Toolbar exclusion failures are best effort. All toolbar overlay popovers use a
scoped show guard when the toolbar is excluded, including drawing groups, style
editors, color pickers, and audio gain controls created during recording. On
Windows, display affinity is applied before showing each surface. On macOS, the
native surface stays hidden until its required window ID is acknowledged, and
remains hidden if the filter update fails. Scoped guards retain native identities
and cover nested popovers even while Qt replaces their native ownership handles.
Acknowledgment belongs to each native window ID: an approved parent remains
visible while a child waits or its filter update fails. Filter submissions still
contain all live excluded windows, so partial updates preserve existing exclusions.
Removals reconcile against the last submitted filter as well as acknowledged IDs;
obsolete completions cannot authorize a replacement or reopen a cancelled popup.

Windows uses `WDA_EXCLUDEFROMCAPTURE`. macOS 15+ saves the actual NSWindow sharing
policy and temporarily sets it to `NSWindowSharingNone`; **ScreenCaptureKit content
filters enforce omission in Snow Shot's captures**. Changing an NSWindow sharing
policy alone does not guarantee omission from third-party ScreenCaptureKit apps.
Restoration preserves the original policy, including a pre-existing `None` policy.
The native state retains the original NSWindow until restoration and is also
cleaned up when the owning QWidget is destroyed. Qt window access is GUI-thread
only; offscreen Qt platforms return unavailable without interpreting fake handles.

The shared `SnowCaptureExclusions` C structure is accepted by desktop, monitor,
region and continuous stream creation, and direct recording. Arrays are copied
before creation returns, sorted and deduplicated, with a 4096-entry limit per
input array. Empty lists may have null pointers. These IDs are macOS WindowServer
IDs and process IDs, never NSView pointers. Streams retain their exclusion
snapshot through pauses and native stream recreation. Direct recording also
supports asynchronous updates through `snow_recording_session_request_exclusions`.
The request copies IDs and returns a generation immediately. On macOS, a worker
enumerates all windows and verifies every required ID before updating all active
ScreenCaptureKit filters. `snow_recording_session_exclusion_status` reports the
requested/applied generations and pending or failed status. Show a new popup only
after its generation is applied. Updates while paused configure filters for the
next stream startup. Successful updates discard queued frames and cached desktop
images from the previous filters. Independent-window capture rejects nonempty filters.
Windows continues to use display affinity; the macOS lists do not implement
filtering for Windows backends.

Stream configuration version is 2. Direct recording configuration version 6
introduced exclusions; current callers use version 11, which adds independent
`system_audio_gain_db` and `microphone_gain_db` fields in the range -24..24 dB.
Versions 1 through 10 remain supported and default both gains to 0 dB.
Recompile users of the expanded unversioned desktop/monitor/region configurations.
Previous version decoding remains bounded by the supplied version and struct size.

Build and run the relevant checks:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-window-capture-exclusion-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-macos-window-capture-exclusion-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-screen-recording-controller-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-scrolling-capture-exclusion-tests
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-(window-capture-exclusion|screen-recording-controller|recording-capture-exclusion|scrolling-capture-exclusion)-tests$' --output-on-failure
# Native policy tests need a Cocoa desktop. Pixel tests additionally need existing
# Screen Recording permission; they return 77 when permission is unavailable.
ctest --test-dir build/snow-shot-macos-arm64-performance \
  -R '^snow-shot-macos-(window-capture-exclusion(-pixels)?|recording-capture-exclusion)-tests$' --output-on-failure
```

The offscreen test verifies independent failures, visibility, successful-ID
selection, duplicate exclusion, reverse-order restoration and destructor cleanup.
The Cocoa test verifies native identity, repeated exclusion, policy restoration
and QWidget destruction. Its pixel mode checks a colored overlay above a known
background through generic region snapshot and continuous stream C APIs, and
verifies capture visibility returns after restoration. The recording controller
test verifies the toolbar preference reaches the C configuration and native
sharing is restored after failure, stop and controller destruction. It also
exercises the platform-independent popup acknowledgment state and real nested
popup lifecycles with delayed/failed updates, superseded generations, rejected
requests, native replacement, removal and cancellation. The scrolling pipeline
test exercises actual asynchronous source creation, mode replacement, export
pause/resume, failed replacement and destruction with every combination
of successful and failed window exclusions. It checks each C stream configuration
and verifies streams are joined before restoration. Run the native tests on
both Apple Silicon and Intel and on macOS 15 before platform-wide qualification;
one machine does not qualify other OS versions or mixed-display configurations.

## Native audio gain qualification

Run on Windows with DXGI and WGC, and on macOS 15+ with ScreenCaptureKit. Use a
constant-level source, enable system audio and microphone, and inspect the exported
MP4 as well as the live UI. Repeat with mixed audio and separate audio tracks.

- In ready, recording and paused states, hover each enabled audio button. Gain
  starts at 0 dB in the middle; −12/+12 dB lowers/raises the processed peak and the
  exported audio. The thumb stays at the gain position while the track follows
  audio. Check Reset, arrow/Page/Home/End keys, drag beyond the popover, clipping,
  Escape without stopping recording, outside click and application deactivation.
  An idle audio-off source permits gain editing with a zero meter. A source not
  captured by the active session stays disabled. GIF/WebP expose no gain popover.
- With **Capture toolbar in recording** on, the toolbar and visible gain popover
  appear in captured pixels. With it off, both stay usable on screen and are
  absent from every exported frame. Open/close and alternate sources repeatedly;
  no old meter value, native window flash or hidden preview should persist.
- Move the toolbar between monitors with different scales, then disconnect and
  reconnect its monitor while the popover is open. Topology changes close the
  popover before Qt moves native windows. Reopening must exclude the replacement
  HWND/NSWindow before showing it; the first exported frame must remain clean.
- Pause with each source enabled, change its gain and open its meter, then resume.
  The paused meter continues to respond, no audio is added to the paused export,
  and resumed audio uses the new gain. Any paused exclusion update must also apply
  to streams created on resume or after a capture-source topology change.
- Using a debugger or instrumented native build, defer exclusion completion,
  close/reopen the popover, replace its native ID, and deliver an old completion.
  A macOS acknowledgment for an older generation or different IDs must not expose
  the current surface. While hidden, no audio meter timer or preview is active.
- Fail Windows display affinity, or on macOS fail enumeration of a required
  hidden window and fail one of several ScreenCaptureKit filter updates. The
  popover remains hidden with one visible error; partial success never releases
  the show guard. Remove the fault and reopen to verify retry. Updated filters
  must discard queued/cached frames from their previous exclusion snapshot.
- Stop, fail startup, close during pending exclusion, and destroy the controller
  with a popover open. No late acknowledgment reopens it, no preview capture or
  polling survives, and original native capture-sharing policies are restored.
  Start another recording to verify gain persistence and fresh exclusion state.

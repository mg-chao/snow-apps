# Follow mouse recording

The **Follow mouse** toggle is immediately before **Record webcam**. It starts off and remembers
the last saved choice. Hover over the control or focus it and press Down or Alt+Down to open its
options. **Smooth movement** and **Reduce jitter** start checked and save independently. The
recording uses the choices made before the countdown. Automation's `follow_mouse` start option
overrides the toggle for that session and uses the saved motion options.

The selected rectangle determines the nominal capture extent and output aspect ratio. Capture
starts centered on the cursor and clamps to the monitor bounds. Reduce jitter holds the crop while
the cursor is within its central half. Entering an outer quarter triggers a recenter. Smooth
movement eases the crop toward its target. A fast cursor jump can temporarily leave the frame.
With both options off, the crop immediately centers on each cursor sample.

If the nominal extent exceeds the monitor, it shrinks uniformly to fit and scales into the original
encoded dimensions. Raster rounding can differ by less than one source pixel per axis. Moving onto
a larger monitor restores the nominal extent. Cursor visibility does not affect following.

Follow mode keeps the area border visible throughout recording. It follows the desktop rectangle
of the latest frame accepted by the recording output, including smoothing, monitor clamping, and
source resizing. The border freezes at the last captured rectangle during pause and finalization,
then returns to the original selection after stopping or a failed start. The toolbar stays in place.
The recording area is excluded from capture on a best-effort basis, independently of the toolbar
capture setting; an exclusion failure never hides the border or prevents recording. Active
following hides retained canvas annotations without changing their contents, history, or viewport.
While preparing a follow recording, you can move or resize the recording area, browse area
history, and adjust the webcam layout. Drawing tools are disabled while the mode is selected.
Mouse, keyboard, and webcam effect previews continue until countdown or capture startup.
Recording controls, shortcuts, auto-hide controls, settings, and countdown remain available.
Trimming uses the existing preview independently of the saved mode.

## Capture lifetime

Following runs at capture cadence below the recording workers. Monitor layouts are cached and
invalidated by native display notifications. A source is prepared on its native owner thread;
the previous monitor keeps recording until the destination supplies a valid frame. A switch
requires current cursor and topology validation and settled exclusion filters. Obsolete work is
canceled and retired before another preparation starts, bounding native sources to two.
Paused sources wait for control notifications instead of polling. Active capture retains precise
Windows sleeps and checks control changes without consuming a capture slot.

Native source resizes invalidate their prepared monitor geometry and force fresh display
discovery, even before a display notification arrives. Startup retries share one deadline;
running sessions retain their admitted image while a replacement source prepares. Invalid
requests, permission failures, and output-device loss keep their normal error handling.

Windows keeps the recording's D3D11 device and encoder alive across monitor changes. Device loss
returns to the recording worker's software fallback instead of retrying the failed output device.
WGC crops its canonical source before HDR conversion, while follow-enabled DXGI retains a canonical
monitor texture so a moving crop works even on a static desktop. Cross-adapter DXGI transfers only
the crop through bounded nonblocking readback and upload pools. Explicit exclusion filters retain
the CPU capture path's filter semantics. Fixed recordings keep their existing capture path.
Native content identities survive replacement of queued observations, including a changed frame
replaced by its subsequent duplicate. CPU queue drops retain the standard recording notifications.

macOS retains full-display ScreenCaptureKit surfaces and uses the Metal/Core Image compositor to
crop the viewport into the fixed output canvas. Panning does not reconfigure the native stream or
reset the output schedule.
Failed sources wait for replacement progress, cancellation, or the next retry/capture deadline
so recovery does not repeatedly poll a failed native stream.

Each admitted frame carries its actual desktop transform. Cursor observations remain attached to
that frame. The nonblocking `snow_recording_session_capture_viewport` query exposes the last
successfully submitted frame's rectangle and a session-local geometry revision. The UI polls it
every 16 ms while recording, retaining its current border when no snapshot is available.
Click and trail history reprojects through the moving viewport without renewing its
age or releasing held keys. Deferred input format 3 adds streamed viewport reprojections; readers
continue to accept formats 1 and 2. The media footer retains the initial canvas geometry rather
than accumulating an in-memory entry for every pan. C recording configuration version 19 adds the
two motion flags. Version 18 uses immediate centering when following is enabled. Versions 1 through
17 use fixed capture.

Deferred replay uses the encoded source's color matrix and range, keeping small cropped
recordings consistent with direct output.

## Focused validation

Build the affected C++ targets, then run filtered CTest cases for follow mode, recording settings,
shortcuts, and the follow icon. From `snow-crates/`, Rust follow cases can be selected with:

```powershell
cargo test -p snow-media follow_mouse
cargo test -p snow-recording-runtime -p snow-recording-c -p snow-recording-effects -p snow-recording-model follow_mouse
cargo test -p snow-capture follow_mouse
cargo test -p snow-macos --test follow_mouse_policy_tests
```

Native capture tests require an interactive desktop and opt into synthetic desktop observations;
they never move the physical mouse. Native macOS capture and compositor execution requires macOS.
The Windows `snow-shot-recording-follow-area-native-tests` CTest case verifies the visible,
click-through border before and after movement, exclusion from WGC and DXGI, and restoration.
Run it explicitly with `ctest --test-dir build/windows-msvc-debug -C Debug
-R '^snow-shot-recording-follow-area-native-tests$' --output-on-failure`.

The Release performance harness accepts `-FollowMouse off|stationary|pan|boundary`. Add
`-FollowMouseSmoothMovement` and `-FollowMouseReduceJitter` to measure the application defaults.
It drives synthetic desktop positions without moving the physical mouse and reports preparation count,
switch count, peak native sources, preparation latency, CPU, memory, and frame statistics. Boundary
mode requires at least two monitors and writes an explicit skip artifact otherwise. Follow-mode
validation decodes output dimensions, presentation timestamps, nonblank content, and spatial
changes; fixed-mode runs retain their source-marker freshness checks.

Use `windows-msvc-performance` through `scripts/run-realtime-recording-perf.ps1`; never use a Debug
or packaging preset for performance measurements. Static workloads distinguish viewport movement
from desktop updates. Compare matching settings and inspect the generated CSV and session reports.

The visible border has a separate UI benchmark; the Rust recording harness does not instantiate it.
Build `snow-shot-screen-recording-area-window-tests` with `windows-msvc-performance`, then run
`ctest --test-dir build/windows-msvc-performance -C Release
-R '^snow-shot-recording-follow-performance-benchmark$' -V`. It exercises stationary and fractional
panning viewports at 1080p and 4K, with one warmup and three measured samples. JSON output includes
elapsed time, resize events, paint events, painted logical pixels, and full viewport layout passes.
The registered benchmark uses offscreen rendering at 125% DPI. To include native window operations,
run the same Release executable with `--follow-performance -platform windows` (or `cocoa` on macOS).
These timings measure UI submission and event processing, not compositor presentation latency.
